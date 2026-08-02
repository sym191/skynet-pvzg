local core = require "skynet.core"

local functions = {
    "send", "genid", "redirect", "command", "intcommand",
    "addresscommand", "error", "harbor", "callback", "trace",
    "tostring", "pack", "unpack", "packstring", "trash", "now", "hpc",
}

for _, name in ipairs(functions) do
    assert(type(core[name]) == "function", "missing skynet.core." .. name)
end

assert(core.addresscommand("REG") == 0x01000001)
assert(core.command("GETENV", "compat_marker") == "available")
assert(core.intcommand("STARTTIME") > 0)

local packed, size = core.pack(nil, true, 42, "hello", { key = "value" })
local first, second, third, fourth, fifth = core.unpack(packed, size)
assert(first == nil and second == true and third == 42 and fourth == "hello")
assert(fifth.key == "value")
core.trash(packed, size)

local packed_string = core.packstring("binary", 99)
local string_value, number_value = core.unpack(packed_string)
assert(string_value == "binary" and number_value == 99)

local harbor, remote = core.harbor(0x02000001)
assert(harbor == 2 and remote == true)
assert(type(core.now()) == "number" and type(core.hpc()) == "number")

debug.getregistry().memlimit = 64 * 1024 * 1024

core.callback(function(message_type, message, message_size, session, source)
    callback_type = message_type
    callback_payload = core.tostring(message, message_size)
    callback_session = session
    callback_source = source
end)

function send_compatible_message()
    return core.send(0x01000002, 10, nil, "outbound")
end
