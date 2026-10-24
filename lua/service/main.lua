local skynet = require "skynet.manager"

skynet.start(function()
	local ok, err = pcall(function()
		local battle_name = skynet.getenv("team_battle_service")
		local battle = battle_name and battle_name ~= "" and skynet.newservice(battle_name) or nil
		local team = skynet.newservice("team/service")
		assert(skynet.call(team, "lua", "init", battle), "team initialization failed")
		skynet.name(".team", team)
		skynet.error("TEAM_SERVICE_READY .team")
		local login = skynet.newservice("login/service", team)
		skynet.name(".login", login)
		skynet.error("LOGIN_SERVICE_READY .login")
	end)

	if not ok then
		skynet.error("TEAM_SERVER_START_FAILED", err)
		skynet.abort()
		return
	end

	skynet.exit()
end)
