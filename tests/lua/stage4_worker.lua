local skynet = require "skynet"
local socket = require "skynet.socket"
local listeners = {}
local finished = {}
local commands = {}

function commands.listen(host, mode, target)
    local listener, address, port = socket.listen(host, 0)
    listeners[#listeners + 1] = listener
    socket.start(listener, function(fd)
        if mode == "transfer" then
            -- Accepted sockets have not started reading; transfer before start.
            skynet.call(target, "lua", "take", fd)
            return
        end
        assert(socket.start(fd))
        if mode == "tail" then
            assert(socket.write(fd, string.rep("t", 4 * 1024 * 1024)))
            socket.close(fd)
        elseif mode == "retire" then
            skynet.send(target, "lua", "accepted")
        else
            commands.echo(fd)
        end
        finished[mode] = (finished[mode] or 0) + 1
    end)
    return address, port
end

function commands.echo(fd)
    while true do
        local line = socket.readline(fd)
        if not line or line == "DROP" then break end
        assert(socket.write(fd, {line, "\n"}))
    end
    socket.close(fd)
end

function commands.take(fd)
    assert(socket.start(fd))
    -- Transfer a socket that has already started: second start must confirm.
    socket.abandon(fd)
    assert(socket.start(fd))
    skynet.fork(commands.echo, fd)
    return true
end

function commands.write(fd, data)
    -- The socket is owned by a different service.
    return socket.write(fd, data)
end

function commands.stop()
    for _, fd in ipairs(listeners) do socket.close(fd) end
    listeners = {}
    return true
end

function commands.finished(mode) return finished[mode] or 0 end

skynet.start(function()
    skynet.dispatch("lua", function(_, _, command, ...)
        skynet.retpack(assert(commands[command])(...))
    end)
end)
