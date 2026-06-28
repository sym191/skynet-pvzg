local skynet = require "skynet"

skynet.start(function()
    skynet.error("skynet-pvzg shared Lua bootstrap")
    skynet.exit()
end)
