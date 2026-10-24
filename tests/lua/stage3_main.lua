local skynet = require "skynet.manager"
local core = require "skynet.core"
local datacenter = require "skynet.datacenter"

skynet.start(function()
    local ok, err = xpcall(function()
        assert(STAGE3_PRELOAD == true)
        assert(skynet.localname ".launcher")
        assert(skynet.localname ".cslave")
        assert(skynet.getenv "harbor" == "0")
        local worker = assert(skynet.newservice("stage3_worker"))
        local data = table.pack(skynet.call(worker, "lua", "echo", nil, false, true,
            0, -1, 2^40, 1.25, "a\0b", {1, 2, 3}))
        assert(data.n == 9 and data[1] == nil and data[2] == false and data[3] == true)
        assert(data[6] == 2^40 and data[7] == 1.25 and data[8] == "a\0b" and data[9][3] == 3)
        assert(skynet.call(worker, "lua", "later") == "later")
        local limited = skynet.newservice("stage3_worker", "memlimit")
        assert(skynet.call(limited, "lua", "allocate") == true)
        assert(skynet.call(limited, "lua", "echo", "after-oom") == "after-oom")
        skynet.kill(limited)
        assert(not pcall(skynet.call, worker, "lua", "bad"))
        assert(not pcall(skynet.newservice, "stage3_worker", "loader_fail"))
        assert(not pcall(skynet.newservice, "stage3_missing_service"))
        assert(not pcall(core.unpack, "\255"))
        assert(core.send(".missing", skynet.PTYPE_TEXT, 0, "") == nil)
        local unique = skynet.uniqueservice("stage3_worker", "unique")
        assert(unique == skynet.uniqueservice("stage3_worker", "unique"))
        assert(unique == skynet.queryservice("stage3_worker"))
        datacenter.set("stage3", "value", { text = "hello", number = 23 })
        assert(datacenter.get("stage3", "value").number == 23)

        -- Global name messages queue before registration, then preserve RPC metadata.
        skynet.timeout(1, function() skynet.name("STAGE3", worker) end)
        assert(skynet.call("STAGE3", "lua", "echo", "global") == "global")
        local forward = skynet.newservice("stage3_forward", worker)
        assert(skynet.call(forward, "lua", "echo", "forward") == "forward")
        local native = assert(skynet.launch("nativeprobe"))
        local native_text, native_number = skynet.call(native, "lua", "native", 42)
        assert(native_text == "native" and native_number == 42)
        assert(not skynet.launch("nativeprobe", "fail"))
        skynet.kill(native)

        local spinner = skynet.newservice("stage3_worker")
        skynet.timeout(5, function() core.command("SIGNAL", skynet.address(spinner) .. " 0") end)
        assert(not pcall(skynet.call, spinner, "lua", "spin"))
        assert(skynet.call(spinner, "lua", "echo", "after-signal") == "after-signal")
        skynet.kill(spinner)

        local co = coroutine.running()
        skynet.fork(function() skynet.sleep(1); skynet.wakeup(co) end)
        skynet.wait(co)
        local profile = require "skynet.profile"
        local wrapped = coroutine.wrap(function()
            profile.start()
            coroutine.yield("yielded")
            return profile.stop()
        end)
        assert(wrapped() == "yielded")
        assert(wrapped() >= 0)
        local doomed = skynet.newservice("stage3_worker")
        assert(not pcall(skynet.call, doomed, "lua", "exit"))
        skynet.kill(forward)
        skynet.kill(worker)
        skynet.error("STAGE3_LOG_MARKER")
        -- Original ABORT retires the logger immediately. Its custom logger uses
        -- a barrier; the refactor deliberately tests immediate ABORT and drain.
        if skynet.getenv("stage3_custom_logger") and not skynet.getenv("stage3_log_drain") then
            assert(skynet.call(skynet.localname(".logger"), "lua", "barrier") == "barrier")
        end
        print("STAGE3_OK")
    end, debug.traceback)
    if not ok then print("STAGE3_FAILED: " .. tostring(err)) end
    skynet.abort()
end)
