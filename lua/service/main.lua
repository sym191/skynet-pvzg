local skynet = require "skynet"
require "skynet.manager"

skynet.start(function()
    skynet.uniqueservice("team_battle")
    skynet.uniqueservice("team")
    skynet.error("skynet-pvzg team services started")
end)
