local skynet = require "skynet.manager"
local core = require "skynet.core"
local target = assert(tonumber(...))
skynet.register_protocol {
    name = "rawforward", id = 100,
    unpack = function(msg, size) return msg, size end,
    dispatch = function(session, source, msg, size)
        core.redirect(target, source, skynet.PTYPE_LUA, session, msg, size)
    end,
}
skynet.forward_type({ [skynet.PTYPE_LUA] = 100 }, function() end)
