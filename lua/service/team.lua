local skynet = require "skynet"
require "skynet.manager"
local json = require "team.json"
local Model = require "team.model"
local protocol = require "team.protocol"

local function get_positive_integer(name, default_value)
	local value = tonumber(skynet.getenv(name) or "")
	if not value or value <= 0 or math.type(value) ~= "integer" then
		return default_value
	end
	return value
end

local function decode_payload(payload)
	if type(payload) == "table" then
		return payload
	elseif type(payload) == "string" then
		local ok, value = pcall(json.decode, payload)
		if ok then
			return value
		end
	end
	return nil
end

local function append_deliveries(target, source)
	for _, delivery in ipairs(source or {}) do
		target[#target + 1] = delivery
	end
end

skynet.start(function()
	local model = Model.new({
		invite_ttl_seconds = get_positive_integer("team_invite_ttl_seconds", 60),
		invite_permission = skynet.getenv("team_invite_permission") or "all",
		leader_succession = skynet.getenv("team_leader_succession") or "online_first",
		max_members = 5,
	})

	local battle_service_name = skynet.getenv("team_battle_service")
	local battle_address
	if battle_service_name and battle_service_name ~= "" then
		battle_address = skynet.uniqueservice(battle_service_name)
	end

	local function create_battle(request)
		if not battle_address then
			return nil, "战斗服务未配置"
		end
		local ok, reply = pcall(skynet.call, battle_address, "lua", "create_team_battle", request)
		if not ok then
			return nil, tostring(reply)
		end
		if type(reply) ~= "table"
			or type(reply.battle_id) ~= "string" or reply.battle_id == ""
			or type(reply.side_id) ~= "string" or reply.side_id == "" then
			return nil, "战斗服务返回的入口无效"
		end
		local entry = {
			battle_id = reply.battle_id,
			team_id = request.team_id,
			side_id = reply.side_id,
			player_ids = protocol.array(request.player_ids),
		}
		return entry
	end

	local handlers = {}

	function handlers.connect(principal)
		return model:connect(principal)
	end

	function handlers.disconnect(player_id)
		return model:disconnect(player_id)
	end

	function handlers.request(player_id, message_id, request_id, payload)
		local decoded_payload = decode_payload(payload)
		local result = model:handle_request(player_id, message_id, request_id, decoded_payload)
		if result.battle_request then
			local entry, error_message = create_battle(result.battle_request)
			if not entry then
				return model:rollback_battle_start(
					result.battle_request.team_id,
					request_id,
					"战斗创建失败：" .. tostring(error_message)
				)
			end
			local attached = model:attach_battle(result.battle_request.team_id, entry)
			if not attached.ok then
				return model:rollback_battle_start(
					result.battle_request.team_id,
					request_id,
					"战斗入口绑定失败"
				)
			end
			append_deliveries(result.deliveries, attached.deliveries)
			result.battle_request = nil
		end
		return result
	end

	function handlers.sync(player_id)
		return model:sync(player_id)
	end

	function handlers.set_battle_eligibility(player_id, can_enter_battle, reason)
		return model:set_battle_eligibility(player_id, can_enter_battle, reason)
	end

	function handlers.release_battle(team_id)
		return model:release_battle(team_id)
	end

	function handlers.status()
		return model:status()
	end

	skynet.dispatch("lua", function(session, source, command, ...)
		local handler = handlers[command]
		if not handler then
			skynet.retpack({
				error = "unknown_command",
				message = "未知的 Team 服务命令",
			})
			return
		end

		local args = table.pack(...)
		local ok, result = xpcall(function()
			return handler(table.unpack(args, 1, args.n))
		end, debug.traceback)
		if not ok then
			skynet.error("[team] request failed: " .. tostring(result))
			skynet.retpack({
				error = "internal_error",
				message = "服务器内部错误",
			})
			return
		end
		skynet.retpack(result)
	end)

	skynet.register("team")
	skynet.error("[team] service ready")
end)
