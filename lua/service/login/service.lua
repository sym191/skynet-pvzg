local skynet = require "skynet.manager"
local queue = require "skynet.queue"
local Player = require "player.model"

local team = assert(tonumber(...), "missing team service")
local players = {}
-- Creation and teardown yield. Serialize registry operations so concurrent
-- logins never publish two services for the same player.
local serial = queue()
local commands = {}

local function lookup(id)
	local address = players[id]
	if address then
		local ok, data = pcall(skynet.call, address, "lua", "get_data")
		if ok and data and data.player_id == id then
			return address
		end
		players[id] = nil
		pcall(skynet.kill, address)
	end
end

function commands.login(_, id)
	if not Player.valid_id(id) then
		return nil, "invalid_player_id"
	end
	local existing = lookup(id)
	if existing then
		return existing
	end

	local address
	local ok, err = pcall(function()
		address = skynet.newservice("player/service", skynet.self(), team)
		assert(skynet.call(address, "lua", "init", id), "player initialization failed")
	end)
	if not ok then
		if address then
			pcall(skynet.kill, address)
		end
		skynet.error("player login failed:", err)
		return nil, "initialization_failed"
	end

	-- Publish only after the player data and durable team binding are ready.
	players[id] = address
	return address
end

function commands.get(_, id)
	if not Player.valid_id(id) then
		return nil, "invalid_player_id"
	end
	return lookup(id)
end

function commands.logout(_, id, expected_address)
	if not Player.valid_id(id) then
		return false, "invalid_player_id"
	end
	local address = players[id]
	if not address then
		return true
	end
	-- A delayed disconnect from an old connection cannot destroy its successor.
	if expected_address ~= address then
		return false, "stale_player_service"
	end
	if not skynet.call(address, "lua", "shutdown") then
		return false, "logout_failed"
	end
	players[id] = nil
	skynet.kill(address)
	return true
end

function commands.replaced(source, id)
	if players[id] == source then
		players[id] = nil
		skynet.kill(source)
	end
end

skynet.start(function()
	skynet.dispatch("lua", function(session, source, command, ...)
		local handler = commands[command]
		local ok, result
		if handler then
			local args = table.pack(...)
			ok, result = pcall(function()
				return table.pack(serial(handler, source, table.unpack(args, 1, args.n)))
			end)
		end
		if not ok then
			skynet.error("login command failed:", command, result)
			result = table.pack(nil, handler and "internal_error" or "unknown_command")
		end
		if session ~= 0 then
			skynet.retpack(table.unpack(result, 1, result.n))
		end
	end)
end)
