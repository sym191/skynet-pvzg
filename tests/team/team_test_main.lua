local skynet = require "skynet"
require "skynet.manager"
local team_service_test = require "team_service_test"

skynet.start(function()
	local ok, message = xpcall(team_service_test.run, debug.traceback)
	if ok then
		skynet.error("TEAM_INTEGRATION_PASS")
	else
		skynet.error("TEAM_INTEGRATION_FAIL\n" .. tostring(message))
	end
	skynet.timeout(1, function()
		skynet.abort()
	end)
end)
