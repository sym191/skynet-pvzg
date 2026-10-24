local skynet = require "skynet.manager"
local socket = require "skynet.socket"
local driver = require "skynet.socketdriver"
local refactor = skynet.getenv("stage5_variant") == "refactor"
local gates, opened, closed, forwarded = {}, {}, {}, {}

local function wait_until(predicate, message)
    local deadline = skynet.now() + 500
    while not predicate() do
        assert(skynet.now() < deadline, message)
        skynet.sleep(1)
    end
end
local function stat(id)
    for _, entry in ipairs(socket.netstat()) do
        if entry.id == id then return entry end
    end
end
local function unused_port()
    local listener, _, port = socket.listen("127.0.0.1", 0)
    socket.close(listener)
    wait_until(function() return not stat(listener) end, "listener close")
    return port
end
local function udp_contract()
    local port = unused_port()
    local received = {}
    local server
    server = socket.udp_listen("127.0.0.2", port, function(data, address)
        assert(#address == 7 and address:byte(1) == 1)
        local host, source_port = socket.udp_address(address)
        assert((host == "127.0.0.1" or host == "127.0.0.2") and source_port > 0)
        assert(socket.sendto(server, address, data))
    end)
    local client = socket.udp(function(data) received[#received + 1] = data end, "127.0.0.2", 0)
    socket.udp_connect(client, "localhost", port)
    socket.udp_connect(client, "127.0.0.2", port)
    local total = 0
    for _, payload in ipairs({"", "a\0b", string.rep("z", 65507)}) do
        received = {}
        assert(socket.write(client, payload))
        wait_until(function() return #received == 1 end, "UDP response timeout")
        assert(received[1] == payload)
        total = total + #payload
    end
    local info = assert(stat(client))
    assert(info.type == "UDP" and info.address == skynet.self())
    assert(info.read == total and info.write == total and info.wbuffer == 0)
    assert(info.peer == "127.0.0.2:" .. port)
    assert(type(info.reading) == "boolean" and type(info.writing) == "boolean")
    assert(type(info.rtime) == "number" and type(info.wtime) == "number")
    assert(info.rtime <= skynet.now() and info.wtime <= skynet.now())
    assert(skynet.now() - info.rtime < 50 and skynet.now() - info.wtime < 50)
    received = {}
    local dial = socket.udp_dial("127.0.0.2", port, function(data) received[#received + 1] = data end)
    assert(socket.lwrite(dial, "dial"))
    wait_until(function() return #received == 1 end, "UDP dial response timeout")
    assert(received[1] == "dial")
    assert(not pcall(driver.udp_listen, "127.0.0.1", 0))
    assert(not pcall(driver.udp_connect, client, "bad host name", port))
    assert(not pcall(driver.udp, "bad host name", port))
    assert(not pcall(driver.udp_dial, "bad host name", port))
    if refactor then
        assert(not socket.write(client, string.rep("x", 65508)))
        assert(not pcall(driver.udp_address, ""))
        assert(not pcall(driver.udp_address, "\1\0"))
        assert(not pcall(driver.udp_send, client, "\1", "bad"))
        assert(not pcall(driver.bind, -1))
    end
    local addresses = socket.resolve("localhost")
    local ipv4 = false
    for _, address in ipairs(addresses) do if address == "127.0.0.1" then ipv4 = true end end
    assert(ipv4)
    assert(not pcall(socket.resolve, "bad host name"))
    -- Exercise the udp(host,port) binding path independently of udp_listen.
    local bound = socket.udp(function() end, "127.0.0.1", unused_port())
    for _, id in ipairs({bound, dial, client, server}) do
        socket.close(id)
        wait_until(function() return not stat(id) end, "UDP cleanup")
    end
end
local function gate_contract(header)
    local port = unused_port()
    local gate = assert(skynet.launch("gate", header, skynet.address(skynet.self()), "127.0.0.1:" .. port, 0, 8))
    gates[gate] = header
    local client = assert(socket.open("127.0.0.1", port))
    wait_until(function() return opened[gate] end, "gate watchdog open")
    local id = opened[gate]
    local format = header == "S" and ">s2" or ">s4"
    local payloads = {"watchdog", "", "sticky", string.rep("p", 60000)}
    local wire = ""
    for _, payload in ipairs(payloads) do wire = wire .. string.pack(format, payload) end
    assert(socket.write(client, wire:sub(1, 1)))
    skynet.sleep(2)
    assert(socket.write(client, wire:sub(2)))
    for _, payload in ipairs(payloads) do
        if #payload > 0 then
            local size = string.unpack(header == "S" and ">I2" or ">I4", assert(socket.read(client, header == "S" and 2 or 4)))
            assert(size == #payload and socket.read(client, size) == payload)
        end
    end
    local tcp = assert(stat(client))
    assert(tcp.type == "TCP" and tcp.address == skynet.self() and tcp.write == #wire)
    assert(tcp.read > 60000 and tcp.wbuffer == 0 and tcp.peer == "127.0.0.1:" .. port)
    local listener_info
    for _, entry in ipairs(socket.netstat()) do
        if entry.address == gate and entry.type == "LISTEN" then listener_info = entry end
    end
    assert(listener_info and listener_info.accept == 1 and listener_info.sock == "127.0.0.1:" .. port)
    -- Command and subsequent network data are synchronized by a round trip.
    skynet.send(gate, "text", "forward " .. id .. " " .. skynet.address(skynet.self()) .. " :123456")
    skynet.sleep(2)
    assert(socket.write(client, string.pack(format, "agent")))
    wait_until(function() return forwarded.agent end, "gate agent forwarding")
    assert(forwarded.agent == 0x123456)
    assert(socket.read(client, 5) == "agent")
    skynet.send(gate, "text", "broker " .. skynet.address(skynet.self()))
    skynet.sleep(2)
    assert(socket.write(client, string.pack(format, "broker")))
    wait_until(function() return forwarded.broker end, "gate broker forwarding")
    assert(forwarded.broker == gate)
    assert(socket.read(client, 6) == "broker")
    if refactor and header == "L" then
        assert(socket.write(client, string.pack(">I4", 0x80000000)))
        assert(socket.readall(client) == "")
    else
        skynet.send(gate, "text", "kick " .. id)
        assert(socket.readall(client) == "")
    end
    socket.close(client)
    wait_until(function() return closed[id] end, "gate watchdog close")
    if refactor and header == "L" then
        for _, length in ipairs({0x1000000, 0xffffffff}) do
            opened[gate] = nil
            local malformed = assert(socket.open("127.0.0.1", port))
            wait_until(function() return opened[gate] end, "malformed gate open")
            local malformed_id = opened[gate]
            assert(socket.write(malformed, string.pack(">I4", length)))
            assert(socket.readall(malformed) == "")
            socket.close(malformed)
            wait_until(function() return closed[malformed_id] end, "malformed gate close")
        end
    end
    skynet.send(gate, "text", "close")
    wait_until(function()
        for _, entry in ipairs(socket.netstat()) do
            if entry.address == gate then return false end
        end
        return true
    end, "gate close listener")
    assert(not socket.open("127.0.0.1", port))
    skynet.kill(gate)
    opened[gate], forwarded.agent, forwarded.broker = nil, nil, nil
end

skynet.register_protocol {name = "text", id = skynet.PTYPE_TEXT, pack = function(data) return data end, unpack = skynet.tostring}
skynet.register_protocol {
    name = "client", id = skynet.PTYPE_CLIENT,
    pack = function(data) return data end, unpack = skynet.tostring,
    dispatch = function(id, source, data)
        forwarded[data] = source
        assert(socket.write(id, data)) -- Allowed across services; gate retains ownership.
    end,
}
skynet.start(function()
    skynet.dispatch("text", function(session, source, text)
        local id, event, rest = text:match("^(%d+) (%w+)%s?(.*)$")
        id = assert(tonumber(id))
        if event == "open" then
            opened[source] = id
            skynet.send(source, "text", "start " .. id)
        elseif event == "close" then
            closed[id] = true
        elseif event == "data" then
            assert(session == id)
            local frame = string.pack(gates[source] == "S" and ">s2" or ">s4", rest)
            -- C gate's PTYPE_CLIENT strips a little-endian socket id trailer.
            skynet.send(source, "client", frame .. string.pack("<I4", id))
        else error("unexpected watchdog event: " .. text) end
    end)
    local ok, err = xpcall(function()
        udp_contract()
        local stdin = socket.stdin()
        assert(socket.readline(stdin) == "stage5 stdin")
        socket.close(stdin)
        assert(not stat(stdin))
        gate_contract("S")
        gate_contract("L")
        print("STAGE5_OK")
    end, debug.traceback)
    if not ok then print("STAGE5_FAILED: " .. tostring(err)) end
    skynet.abort()
end)
