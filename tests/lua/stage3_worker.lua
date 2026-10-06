local skynet = require "skynet"
local mode = ...
if mode == "loader_fail" then error("intentional loader failure") end
if mode == "memlimit" then debug.getregistry().memlimit = 4 * 1024 * 1024 end
skynet.start(function()
    skynet.dispatch("lua", function(session, source, command, ...)
        if command == "echo" then
            skynet.retpack(...)
        elseif command == "later" then
            local response = skynet.response()
            skynet.timeout(1, function() response(true, "later") end)
        elseif command == "exit" then
            skynet.exit()
        elseif command == "allocate" then
            local ok = pcall(string.rep, "x", 8 * 1024 * 1024)
            skynet.retpack(not ok)
        elseif command == "spin" then
            while true do end
        else
            error("intentional dispatch error")
        end
    end)
end)
