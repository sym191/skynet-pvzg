local skynet = require "skynet.manager"
local json = require "common.json"
local Model = require "team.model"
local store = require "team.store"

local battle_service, model, path

-- Only trusted player services call online/offline; authentication belongs to
-- the caller. Subsequent requests are bound to the actual Skynet source address.
local sessions, players = {}, {}

local function flush()
	for _, event in ipairs(model.events) do
		local agent = players[event.player_id]
		if agent then
			skynet.send(agent, "lua", "team_event", event.message, json.encode(event.body))
		end
	end

	for _, job in ipairs(model.jobs) do
		-- Lost RPC replies cannot prove creation failed. Only the provider can
		-- confirm cancellation, using the durable token as its idempotency key.
		skynet.send(battle_service, "lua", "create_team_battle", skynet.self(), job)
	end
end

local function transaction(operation)
	local before = json.encode(model.state)
	model:begin(os.time())

	local ok, result = pcall(function()
		local values = table.pack(operation())
		if json.encode(model.state) ~= before then
			store.save(path, model.state)
		end

		return values
	end)

	if not ok then
		model.state = json.decode(before)
		model.events, model.jobs = {}, {}
		error(result)
	end

	flush()
	return table.unpack(result, 1, result.n)
end

local commands = {}

function commands.init(_, battle_address)
	assert(not model, "team already initialized")
	battle_service = battle_address
	path = assert(skynet.getenv("team_state_path"))

	local options = require(skynet.getenv("team_policy_module") or "team.policy")
	options.invite_ttl = assert(tonumber(skynet.getenv("team_invite_ttl")))
	assert(options.invite_ttl > 0 and options.invite_ttl % 1 == 0)
	options.battle_available = battle_service ~= nil

	model = Model.new(store.load(path), options)
	transaction(function()
		model:recover()
	end)

	skynet.fork(function()
		while true do
			skynet.sleep(100)
			local ok, err = pcall(transaction, function()
				model:expire()
			end)

			if not ok then
				skynet.error("team expiration failed:", err)
			end
		end
	end)

	if battle_service then
		for _, t in pairs(model.state.teams) do
			if t.locked then
				skynet.send(battle_service, "lua", "recover_team_battle", skynet.self(), t.id, t.battle_token, t.entry)
			end
		end
	end

	return true
end

function commands.online(source, profile)
	assert(
		type(profile) == "table"
			and type(profile.player_id) == "string"
			and #profile.player_id > 0
			and #profile.player_id <= 256
			and type(profile.display_name) == "string"
			and type(profile.avatar_key) == "string",
		"invalid player profile"
	)

	local id = profile.player_id
	assert(not sessions[source] or sessions[source] == id, "offline before changing player")

	local previous = players[id]
	sessions[source], players[id] = id, source

	local ok, err = pcall(transaction, function()
		model:connect(profile)
	end)

	if not ok then
		sessions[source], players[id] = previous == source and id or nil, previous
		error(err)
	end

	if previous and previous ~= source then
		sessions[previous] = nil
		skynet.send(previous, "lua", "team_replaced")
	end

	return true
end

function commands.offline(source)
	local id = sessions[source]
	if id and players[id] == source then
		transaction(function()
			model:disconnect(id)
		end)

		players[id] = nil
	end

	sessions[source] = nil
	return true
end

function commands.request(source, message, payload)
	local valid, request = pcall(json.decode, payload)
	if not valid then
		request = false
	end

	local id = sessions[source]
	local response_message, response = transaction(function()
		if not id then
			return model:failure("not_authenticated")
		end

		return model:request(id, message, request)
	end)

	-- Keep JSON array/null semantics across Skynet's table serialization.
	return response_message, json.encode(response)
end

-- Trusted game services only; no client packet routes to these commands.
function commands.set_activity(source, id, reason)
	assert(source == battle_service, "untrusted activity source")
	return transaction(function()
		return model:set_activity(id, reason)
	end)
end

function commands.battle_ready(source, team_id, token, entry)
	assert(source == battle_service, "untrusted battle source")
	return transaction(function()
		return model:battle_ready(team_id, token, entry)
	end)
end

function commands.battle_finished(source, team_id, token)
	assert(source == battle_service, "untrusted battle source")
	return transaction(function()
		return model:battle_finished(team_id, token)
	end)
end

skynet.start(function()
	skynet.dispatch("lua", function(session, source, command, ...)
		local args = table.pack(...)
		local ok, result = pcall(function()
			return table.pack(assert(commands[command], "unknown team command")(source, table.unpack(args, 1, args.n)))
		end)

		if ok then
			if session ~= 0 then
				skynet.retpack(table.unpack(result, 1, result.n))
			end
		else
			skynet.error("team command failed:", command, result)
			if command == "offline" and sessions[source] then
				skynet.abort()
			end

			if session ~= 0 then
				if command == "request" then
					skynet.retpack(
						255,
						json.encode({
							schema_version = 3,
							type = "error",
							ok = false,
							error = "internal_error",
							message = "服务器暂时无法处理，请刷新状态",
						})
					)
				else
					skynet.retpack(false)
				end
			end
		end
	end)
end)
