local skynet = require "skynet.manager"
local json = require "common.json"
local store = require "team.store"

local function run()
	local phase = skynet.getenv("login_test_phase")
	local path = skynet.getenv("team_state_path")
	local previous = store.load(path)
	-- Exercise the production bootstrap and service names.
	skynet.newservice("main")
	local login = skynet.localname(".login")
	for _ = 1, 100 do
		if login then break end
		skynet.sleep(1)
		login = skynet.localname(".login")
	end
	assert(login, "login not registered by bootstrap")
	local team = assert(skynet.localname(".team"))
	local function call(command, ...)
		return skynet.call(login, "lua", command, ...)
	end
	local function request(address, number, fields)
		local types = { [4] = "invite_team", [5] = "respond_invite", [7] = "sync_team" }
		fields = fields or {}
		fields.schema_version, fields.type = 3, types[number]
		local message, payload = skynet.call(address, "lua", "request", number, json.encode(fields))
		assert(message == (number == 7 and 101 or number + 100), payload)
		return json.decode(payload)
	end

	if phase == "rollback" then
		local agent, err = call("login", "a")
		assert(not agent and err == "initialization_failed")
		assert(not call("get", "a"))
		assert(not store.load(path), "failed login published durable player data")
		local services = skynet.call(".launcher", "lua", "LIST")
		for _, name in pairs(services) do
			assert(not name:find("player/service", 1, true), "failed player service leaked")
		end
		assert(os.remove(path .. ".tmp"))
		agent = assert(call("login", "a"))
		assert(request(agent, 7).snapshot.team_id == "")
		assert(call("logout", "a", agent))
		return
	end

	if phase == "recover" then
		assert(not call("get", "a"))
		local agent = assert(call("login", "a"))
		local current = request(agent, 7)
		assert(current.snapshot.team_id == previous.players.a.team_id)
		assert(current.snapshot.state_version > previous.players.a.version)
		assert(#current.snapshot.members == 2)
		assert(current.snapshot.members[1].online)
		assert(not current.snapshot.members[2].online)
		return
	end

	for _, id in ipairs({ false, 123, "", string.rep("a", 257), "a\n", {} }) do
		local agent, err = call("login", id)
		assert(not agent and err == "invalid_player_id")
	end
	local missing, err = call("login")
	assert(not missing and err == "invalid_player_id")
	assert(not call("get", "a"))

	local addresses, remaining, waiter = {}, 8, coroutine.running()
	for i = 1, 8 do
		skynet.fork(function()
			addresses[i] = call("login", "a")
			remaining = remaining - 1
			if remaining == 0 then skynet.wakeup(waiter) end
		end)
	end
	skynet.wait(waiter)
	local a = assert(addresses[1])
	for i = 1, 8 do assert(addresses[i] == a, "duplicate player service") end
	assert(call("get", "a") == a)
	local data = skynet.call(a, "lua", "get_data")
	assert(data.player_id == "a" and data.display_name == "玩家a" and data.avatar_key == "")
	assert(not skynet.call(a, "lua", "init", "other"))
	assert(not skynet.call(a, "lua", "shutdown"))
	assert(not skynet.call(a, "lua", "team_replaced"))
	assert(not skynet.call(a, "lua", "team_event", 110, "forged"))
	assert(skynet.call(a, "lua", "get_data").player_id == "a")
	assert(request(a, 7).snapshot.team_id == "")
	local b = assert(call("login", "b"))
	assert(a ~= b)
	local invite = request(a, 4, { team_id = "", target_player_id = "b" })
	assert(invite.ok)
	assert(request(b, 5, { invitation_id = invite.invitation_id, accept = true }).ok)
	assert(#request(a, 7).snapshot.members == 2)
	local events = skynet.call(b, "lua", "events")
	local invited = false
	for _, event in ipairs(events) do
		if event.message == 110 then invited = true end
	end
	assert(invited, "player did not receive team invitation")
	assert(#skynet.call(b, "lua", "events") == 0)

	assert(call("logout", "b", b))
	assert(not call("get", "b"))
	assert(not request(a, 7).snapshot.members[2].online)
	local new_b = assert(call("login", "b"))
	assert(new_b ~= b)
	local closed, reason = call("logout", "b", b)
	assert(not closed and reason == "stale_player_service")
	assert(call("get", "b") == new_b)
	assert(request(a, 7).snapshot.members[2].online)

	-- A dead player is recreated and the old Team binding is replaced.
	skynet.kill(new_b)
	local recovered_b = assert(call("login", "b"))
	assert(recovered_b ~= new_b and call("get", "b") == recovered_b)
	assert(#request(recovered_b, 7).snapshot.members == 2)
	-- Team's legacy takeover path also removes the old login registration.
	local replacement = skynet.newservice("team/player_agent")
	assert(skynet.call(replacement, "lua", "attach", team, "b"))
	for _ = 1, 100 do
		if not call("get", "b") then break end
		skynet.sleep(1)
	end
	assert(not call("get", "b"))
	assert(call("login", "b"))
	assert(call("get", "a") == a)
	assert(store.load(path).players.a.online)
end

skynet.start(function()
	local ok, err = xpcall(run, debug.traceback)
	skynet.error(ok and "LOGIN_INTEGRATION_PASS" or "LOGIN_INTEGRATION_FAIL", err or skynet.getenv("login_test_phase"))
	skynet.sleep(10)
	skynet.abort()
end)
