local skynet = require "skynet.manager"
local json = require "common.json"
local store = require "team.store"

local kinds = {
	[3] = "leave_team",
	[4] = "invite_team",
	[5] = "respond_invite",
	[6] = "kick_member",
	[7] = "sync_team",
	[8] = "start_battle",
}

local function run()
	local phase = skynet.getenv("team_test_phase")
	local previous = store.load(skynet.getenv("team_state_path"))
	local battle = skynet.newservice("team/battle_provider")
	local team = skynet.newservice("team/service")
	assert(skynet.call(team, "lua", "init", battle))
	local agents = {}

	local function connect(id)
		local agent = skynet.newservice("team/player_agent")
		assert(skynet.call(agent, "lua", "attach", team, id))
		agents[id] = agent
		return agent
	end

	local function request(id, number, fields)
		fields = fields or {}
		fields.schema_version, fields.type = 3, kinds[number]
		local message, payload = skynet.call(agents[id], "lua", "request", number, json.encode(fields))
		local response = json.decode(payload)
		assert(message == (response.ok == false and 255 or number == 7 and 101 or number + 100))
		return response
	end

	local function join(id, team_id)
		local invitation = request("a", 4, { team_id = team_id or "", target_player_id = id })
		assert(invitation.ok)
		assert(request(id, 5, { invitation_id = invitation.invitation_id, accept = true }).ok)
		return invitation.snapshot.team_id
	end

	if phase == "recover" then
		connect("a")
		local current = request("a", 7)
		local old_team = previous.teams[previous.players.a.team_id]
		assert(current.snapshot.team_id == old_team.id and current.snapshot.membership_locked)
		assert(current.snapshot.state_version > previous.players.a.version)
		assert(json.encode(current.entry) == json.encode(old_team.entry))
		connect("c")
		assert(request("c", 7).snapshot.team_id == "")
		local found = false
		for _, event in ipairs(skynet.call(agents.c, "lua", "events")) do
			if event.message == 112 then
				found = true
			end
		end

		assert(found, "offline removal reason lost across restart")
		return
	end

	if phase == "rollback" then
		-- The Python harness creates a directory at the temporary-write path.
		-- No old players were online, so init has nothing to write.
		local agent = skynet.newservice("team/player_agent")
		assert(not skynet.call(agent, "lua", "attach", team, "a"))
		local number, payload = skynet.call(agent, "lua", "request", 7, '{"schema_version":3,"type":"sync_team"}')
		assert(number == 255 and json.decode(payload).error == "not_authenticated")
		return
	end

	local message, payload =
		skynet.call(team, "lua", "request", 7, '{"schema_version":3,"type":"sync_team","player_id":"a"}')
	assert(message == 255 and json.decode(payload).error == "not_authenticated")
	for _, id in ipairs({ "a", "b", "c", "d", "e", "f", "g" }) do
		connect(id)
	end

	local empty = request("a", 7)
	assert(json.encode(empty.snapshot.members) == "[]" and empty.entry == json.null)
	local _, bad = skynet.call(agents.a, "lua", "request", 7, "{")
	assert(json.decode(bad).error == "invalid_request")
	local team_id = join("b")
	join("c", team_id)
	join("d", team_id)
	local ie = request("a", 4, { team_id = team_id, target_player_id = "e" })
	local iff = request("a", 4, { team_id = team_id, target_player_id = "f" })
	assert(request("b", 4, { team_id = team_id, target_player_id = "e" }).invitation_id == ie.invitation_id)
	local results, remaining, waiter = {}, 2, coroutine.running()
	for id, invitation in pairs({ e = ie, f = iff }) do
		skynet.fork(function()
			results[id] = request(id, 5, { invitation_id = invitation.invitation_id, accept = true })
			remaining = remaining - 1
			if remaining == 0 then
				skynet.wakeup(waiter)
			end
		end)
	end

	skynet.wait(waiter)
	assert(results.e.ok ~= results.f.ok)
	assert((results.e.ok and results.f or results.e).error == "team_full")
	assert(#request("a", 7).snapshot.members == 5)
	assert(request("b", 6, { team_id = team_id, target_player_id = "a", player_id = "a" }).error == "not_leader")
	assert(skynet.call(agents.c, "lua", "offline"))
	assert(request("a", 8, { team_id = team_id, template_id = "hold" }).error == "battle_blocked")
	assert(request("a", 6, { team_id = team_id, target_player_id = "c" }).ok)
	request("a", 4, { team_id = team_id, target_player_id = "g" })
	assert(#request("g", 7).invitations == 1)
	skynet.sleep(210)
	assert(#request("g", 7).invitations == 0)
	for _, template in ipairs({ "cancel", "finish" }) do
		assert(request("a", 8, { team_id = team_id, template_id = template }).ok)
		skynet.sleep(template == "cancel" and 30 or 110)
		assert(not request("a", 7).snapshot.membership_locked)
	end

	local ig = request("a", 4, { team_id = team_id, target_player_id = "g" })
	assert(request("a", 8, { team_id = team_id, template_id = "hold" }).ok)
	assert(request("g", 5, { invitation_id = ig.invitation_id, accept = true }).error == "invite_invalid")
	assert(request("b", 3, { team_id = team_id }).error == "membership_locked")
	assert(not skynet.call(team, "lua", "battle_finished", team_id, "guessed-token"))
	skynet.sleep(20)
	local current = request("a", 7)
	assert(current.entry ~= json.null)
	assert(json.encode(current.entry) == json.encode(request("b", 7).entry))
	local old = agents.a
	connect("a")
	assert(skynet.call(old, "lua", "offline"))
	local _, rejected = skynet.call(old, "lua", "request", 7, '{"schema_version":3,"type":"sync_team"}')
	assert(json.decode(rejected).error == "not_authenticated")
	assert(request("a", 7).snapshot.members[1].online)
	assert(json.encode(request("a", 7).entry) == json.encode(current.entry))
end

skynet.start(function()
	local ok, err = xpcall(run, debug.traceback)
	if ok then
		skynet.error("TEAM_INTEGRATION_PASS", skynet.getenv("team_test_phase"))
	else
		skynet.error("TEAM_INTEGRATION_FAIL", err)
	end

	skynet.sleep(10) -- Let the asynchronous logger publish the test result before abort.
	skynet.abort()
end)
