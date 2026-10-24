-- A trusted player-service fixture; there is no network or authentication here.
local skynet = require "skynet"
local json = require "common.json"

local team, events, versions = nil, {}, {}
local commands = {}

local function observe(body)
	local snapshot = body.snapshot
	if snapshot then
		local encoded = json.encode(snapshot)
		assert(
			not versions[snapshot.state_version] or versions[snapshot.state_version] == encoded,
			"same-version snapshot conflict"
		)

		versions[snapshot.state_version] = encoded
	end
end

function commands.attach(address, id)
	team = address
	return skynet.call(team, "lua", "online", {
		player_id = id,
		display_name = "玩家" .. id,
		avatar_key = "",
	})
end

function commands.request(message, payload)
	local number, response = skynet.call(team, "lua", "request", message, payload)
	observe(json.decode(response))
	return number, response
end

function commands.offline()
	return skynet.call(team, "lua", "offline")
end

function commands.team_event(message, payload)
	observe(json.decode(payload))
	events[#events + 1] = { message = message, payload = payload }
end

function commands.team_replaced()
	events[#events + 1] = { message = "replaced" }
end

function commands.events()
	return events
end

skynet.start(function()
	skynet.dispatch("lua", function(session, _, command, ...)
		local result = table.pack(assert(commands[command])(...))
		if session ~= 0 then
			skynet.retpack(table.unpack(result, 1, result.n))
		end
	end)
end)
