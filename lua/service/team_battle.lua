local skynet = require "skynet"
require "skynet.manager"
local protocol = require "team.protocol"

skynet.start(function()
	local next_battle_id = 0

	skynet.dispatch("lua", function(session, source, command, request)
		if command ~= "create_team_battle" then
			skynet.retpack(nil)
			return
		end
		if type(request) ~= "table"
			or type(request.team_id) ~= "string" or request.team_id == ""
			or type(request.player_ids) ~= "table" or #request.player_ids == 0 then
			skynet.retpack(nil)
			return
		end

		next_battle_id = next_battle_id + 1
		local player_ids = protocol.array()
		for _, player_id in ipairs(request.player_ids) do
			player_ids[#player_ids + 1] = player_id
		end
		skynet.retpack({
			battle_id = string.format("battle_%06d", next_battle_id),
			side_id = "side_" .. request.team_id,
			player_ids = player_ids,
		})
	end)

	skynet.register("team_battle")
	skynet.error("[team_battle] service ready")
end)
