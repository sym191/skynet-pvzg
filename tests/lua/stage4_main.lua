local skynet = require "skynet.manager"
local socket = require "skynet.socket"
local driver = require "skynet.socketdriver"
local channel = require "skynet.socketchannel"

local function buffer_contract()
    local buffer, pool = driver.buffer(), {}
    assert(driver.push(buffer, pool, driver.str2p("a\0b\r")) == 4)
    assert(driver.push(buffer, pool, driver.str2p("\nrest")) == 9)
    assert(driver.readline(buffer, pool, "\r\n") == "a\0b")
    assert(driver.pop(buffer, pool, 2) == "re")
    assert(driver.readall(buffer, pool) == "st")
    driver.push(buffer, pool, driver.str2p("discard"))
    driver.clear(buffer, pool)
    driver.drop(driver.str2p("drop"))
    assert(driver.header("\1\2") == 258)
end

skynet.start(function()
    local accepted = false
    skynet.dispatch("lua", function(_, _, command)
        assert(command == "accepted")
        accepted = true
    end)
    local ok, err = xpcall(function()
        buffer_contract()
        local worker = skynet.newservice("stage4_worker")
        local writer = skynet.newservice("stage4_worker")
        local function echo(host)
            local address, port = skynet.call(worker, "lua", "listen", host, "echo")
            assert(address == host and port > 0)
            local fd = assert(socket.open(host, port))
            driver.nodelay(fd)
            socket.pause(fd)
            assert(skynet.call(writer, "lua", "write", fd, "cross-service\n"))
            assert(socket.readline(fd) == "cross-service")
            local payload = string.rep("a\0b", 400000)
            assert(socket.lwrite(fd, payload .. "\n"))
            assert(socket.readline(fd) == payload)
            assert(socket.write(fd, "one\ntwo\n"))
            assert(socket.readline(fd) == "one" and socket.readline(fd) == "two")
            socket.close(fd)
            return port
        end
        local port = echo("127.0.0.1")
        assert(not pcall(socket.listen, "127.0.0.1", port), "occupied listen port succeeded")
        assert(not pcall(socket.listen, "bad host name", 0), "invalid listen host succeeded")
        echo("::1")

        local ch = channel.channel {host = "127.0.0.1", port = port, nodelay = true}
        local function response(sock) return true, sock:readline() end
        assert(ch:request("channel\n", response) == "channel")
        assert(not pcall(ch.request, ch, "DROP\n", response))
        assert(ch:request("reconnected\n", response) == "reconnected")
        ch:close()

        local target = skynet.newservice("stage4_worker")
        local _, transfer_port = skynet.call(worker, "lua", "listen", "127.0.0.1", "transfer", target)
        local fd = assert(socket.open("127.0.0.1", transfer_port))
        assert(socket.write(fd, "transferred\n"))
        assert(socket.readline(fd) == "transferred")
        socket.close(fd)

        local gate = skynet.newservice("stage4_gate")
        local gate_port = skynet.call(gate, "lua", "open", {address = "127.0.0.1", port = 0})
        fd = assert(socket.open("127.0.0.1", gate_port))
        local frames = {"split", "", "sticky", string.rep("p", 60000)}
        local wire = ""
        for _, data in ipairs(frames) do wire = wire .. string.pack(">s2", data) end
        -- Split the first header, then send coalesced frames with a large body.
        assert(socket.write(fd, wire:sub(1, 1)))
        skynet.sleep(2)
        assert(socket.write(fd, wire:sub(2)))
        for _, data in ipairs(frames) do
            local size = string.unpack(">I2", assert(socket.read(fd, 2)))
            assert(size == #data)
            if size > 0 then assert(socket.read(fd, size) == data) end
        end
        socket.close(fd)
        skynet.call(gate, "lua", "close")

        local _, tail_port = skynet.call(worker, "lua", "listen", "127.0.0.1", "tail")
        fd = assert(socket.open("127.0.0.1", tail_port))
        socket.pause(fd) -- explicit slow reader, independent of sanitizer speed
        skynet.sleep(5) -- close must preserve the queued tail
        assert(socket.readall(fd) == string.rep("t", 4 * 1024 * 1024))
        socket.close(fd)
        local deadline = skynet.now() + 100
        while skynet.call(worker, "lua", "finished", "tail") < 1 do
            assert(skynet.now() < deadline)
            skynet.sleep(1)
        end

        local doomed = skynet.newservice("stage4_worker")
        local _, doomed_port = skynet.call(doomed, "lua", "listen", "127.0.0.1", "retire", skynet.self())
        fd = assert(socket.open("127.0.0.1", doomed_port))
        while not accepted do skynet.sleep(1) end
        skynet.kill(doomed)
        -- Refactor additionally closes service-owned sockets at retirement;
        -- the upstream socket module closes them during Lua VM destruction.
        assert(socket.readall(fd) == "")
        socket.close(fd)

        skynet.call(worker, "lua", "stop")
        local failed, why = socket.open("127.0.0.1", port)
        assert(not failed and type(why) == "string")
        failed, why = socket.open("bad host name", 80)
        assert(not failed and type(why) == "string")
        assert(not driver.send(0x7fffffff, "rejected"))
        assert(not driver.send(0x7fffffff, driver.str2p("owned-rejection")))
        print("STAGE4_OK")
    end, debug.traceback)
    if not ok then print("STAGE4_FAILED: " .. tostring(err)) end
    skynet.abort()
end)
