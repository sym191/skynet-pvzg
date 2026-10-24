-- Integration-test provider only. It allocates no playable battle.
local skynet = require "skynet"

local function ready(team_service, team_id, token, members)
	assert(skynet.call(team_service, "lua", "battle_ready", team_id, token, {
		battle_id = "test_" .. token,
		team_id = team_id,
		side_id = "test_side",
		player_ids = members,
	}))
end

skynet.start(function()
	skynet.dispatch("lua", function(_, _, command, team_service, ...)
		if command == "create_team_battle" then
			local job = ...
			if job.template_id == "cancel" then
				skynet.sleep(20)
				assert(skynet.call(team_service, "lua", "battle_finished", job.team_id, job.token))
			else
				skynet.sleep(10)
				ready(team_service, job.team_id, job.token, job.player_ids)
				if job.template_id == "finish" then
					skynet.sleep(80)
					assert(skynet.call(team_service, "lua", "battle_finished", job.team_id, job.token))
				end
			end
		elseif command == "recover_team_battle" then
			local team_id, token, entry = ...
			if entry then
				ready(team_service, team_id, token, entry.player_ids)
			else
				skynet.call(team_service, "lua", "battle_finished", team_id, token)
			end
		else
			error("unexpected test provider command")
		end
	end)
end)
