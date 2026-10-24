local skynet = require "skynet"
local queue = require "skynet.queue"
local Player = require "player.model"

local owner, team = ...
owner, team = assert(tonumber(owner)), assert(tonumber(team))
local data, active
local events = {}
local serial = queue()
local commands = {}

function commands.init(source, id)
	assert(source == owner, "untrusted lifecycle source")
	assert(not data, "player already initialized")
	local initial = Player.new(id)
	assert(skynet.call(team, "lua", "online", initial), "team registration failed")
	data, active = initial, true
	return true
end

function commands.get_data()
	return active and data or nil
end

function commands.request(_, message, payload)
	assert(active, "player is offline")
	return skynet.call(team, "lua", "request", message, payload)
end

function commands.events()
	assert(active, "player is offline")
	local pending = events
	events = {}
	return pending
end

function commands.shutdown(source)
	assert(source == owner, "untrusted lifecycle source")
	if active then
		assert(skynet.call(team, "lua", "offline"), "team disconnect failed")
		active = false
	end
	return true
end

function commands.team_event(source, message, payload)
	assert(source == team, "untrusted team event")
	-- Events may arrive while init is waiting for the online RPC response.
	events[#events + 1] = { message = message, payload = payload }
end

function commands.team_replaced(source)
	assert(source == team, "untrusted team replacement")
	active = false
	skynet.send(owner, "lua", "replaced", data.player_id)
end

skynet.start(function()
	skynet.dispatch("lua", function(session, source, command, ...)
		local args = table.pack(...)
		local ok, result = pcall(function()
			return table.pack(serial(assert(commands[command], "unknown player command"),
				source, table.unpack(args, 1, args.n)))
		end)
		if not ok then
			skynet.error("player command failed:", command, result)
			result = table.pack(nil, "player_command_failed")
		end
		if session ~= 0 then
			skynet.retpack(table.unpack(result, 1, result.n))
		end
	end)
end)
