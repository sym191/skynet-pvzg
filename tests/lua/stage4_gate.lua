local skynet = require "skynet"
local gateserver = require "snax.gateserver"
local netpack = require "skynet.netpack"
local driver = require "skynet.socketdriver"

gateserver.start {
    open = function(_, conf) return conf.port end,
    connect = function(fd) gateserver.openclient(fd) end,
    message = function(fd, pointer, size)
        local data = netpack.tostring(pointer, size)
        assert(driver.send(fd, netpack.pack(data)))
    end,
    disconnect = function(fd) gateserver.closeclient(fd) end,
    error = function(fd) gateserver.closeclient(fd) end,
    command = function(cmd) error("unknown command " .. cmd) end,
}
