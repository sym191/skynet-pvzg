local skynet = require "skynet"

skynet.start(function()
    skynet.error("standard Skynet service started")
    skynet.exit()
end)
