-- JSON values retain the distinction between {}, [] and null.
local M = {}
M.null = setmetatable({}, { __json = "null" })

local array_mt = { __json = "array" }

function M.array(t)
	return setmetatable(t or {}, array_mt)
end

function M.encode(value)
	local seen = {}

	local function encode(v, depth)
		assert(depth < 64, "JSON nesting limit")
		local kind = type(v)
		if v == M.null then
			return "null"
		end

		if kind == "boolean" then
			return tostring(v)
		end

		if kind == "number" then
			assert(v == v and math.abs(v) ~= math.huge, "invalid JSON number")
			return tostring(v)
		end

		if kind == "string" then
			assert(utf8.len(v), "invalid UTF-8")
			return '"'
				.. v:gsub('[%z\1-\31\\"]', function(c)
					return string.format("\\u%04x", c:byte())
				end)
				.. '"'
		end

		assert(kind == "table" and not seen[v], "invalid JSON value")
		seen[v] = true
		local out = {}
		if getmetatable(v) == array_mt then
			for i = 1, #v do
				out[i] = encode(v[i], depth + 1)
			end

			seen[v] = nil
			return "[" .. table.concat(out, ",") .. "]"
		end

		for k, item in pairs(v) do
			assert(type(k) == "string", "JSON object key must be a string")
			out[#out + 1] = encode(k, depth + 1) .. ":" .. encode(item, depth + 1)
		end

		table.sort(out)
		seen[v] = nil
		return "{" .. table.concat(out, ",") .. "}"
	end

	return encode(value, 0)
end

function M.decode(text)
	assert(type(text) == "string" and utf8.len(text), "invalid JSON UTF-8")

	local pos, parse = 1

	local function whitespace()
		local _, last = text:find("^[ \t\r\n]*", pos)
		pos = (last or pos - 1) + 1
	end

	local function codepoint()
		local hex = text:sub(pos, pos + 3)
		assert(#hex == 4 and hex:match("^%x+$"), "invalid unicode escape")
		pos = pos + 4
		return tonumber(hex, 16)
	end

	local function str()
		pos = pos + 1
		local out = {}
		while pos <= #text do
			local c = text:sub(pos, pos)
			pos = pos + 1
			if c == '"' then
				return table.concat(out)
			end

			assert(c:byte() >= 32, "unescaped control character")
			if c == "\\" then
				local e = text:sub(pos, pos)
				pos = pos + 1
				local escapes = {
					['"'] = '"',
					["\\"] = "\\",
					["/"] = "/",
					b = "\b",
					f = "\f",
					n = "\n",
					r = "\r",
					t = "\t",
				}

				if e == "u" then
					local cp = codepoint()
					if cp >= 0xd800 and cp <= 0xdbff then
						assert(text:sub(pos, pos + 1) == "\\u", "missing surrogate")
						pos = pos + 2
						local low = codepoint()
						assert(low >= 0xdc00 and low <= 0xdfff, "invalid surrogate")
						cp = 0x10000 + (cp - 0xd800) * 0x400 + low - 0xdc00
					else
						assert(cp < 0xdc00 or cp > 0xdfff, "unexpected surrogate")
					end

					c = utf8.char(cp)
				else
					c = assert(escapes[e], "invalid JSON escape")
				end
			end

			out[#out + 1] = c
		end

		error("unterminated string")
	end

	parse = function(depth)
		assert(depth < 64, "JSON nesting limit")
		whitespace()
		local c = text:sub(pos, pos)
		if c == '"' then
			return str()
		end

		if c == "{" or c == "[" then
			local array = c == "["
			local result = array and M.array() or {}
			local close = array and "]" or "}"
			pos = pos + 1
			whitespace()
			if text:sub(pos, pos) == close then
				pos = pos + 1
				return result
			end

			while true do
				whitespace()
				local key
				if not array then
					assert(text:sub(pos, pos) == '"', "expected object key")
					key = str()
					assert(result[key] == nil, "duplicate JSON key")
					whitespace()
					assert(text:sub(pos, pos) == ":", "expected colon")
					pos = pos + 1
				end

				result[key or (#result + 1)] = parse(depth + 1)
				whitespace()
				c = text:sub(pos, pos)
				pos = pos + 1
				if c == close then
					return result
				end

				assert(c == ",", "expected comma")
			end
		end

		for literal, value in pairs({ ["true"] = true, ["false"] = false, ["null"] = M.null }) do
			if text:sub(pos, pos + #literal - 1) == literal then
				pos = pos + #literal
				return value
			end
		end

		local start = pos
		if c == "-" then
			pos = pos + 1
		end

		if text:sub(pos, pos) == "0" then
			pos = pos + 1
		else
			local digits = text:match("^[1-9]%d*", pos)
			assert(digits, "expected JSON value")
			pos = pos + #digits
		end

		if text:sub(pos, pos) == "." then
			pos = pos + 1
			local digits = assert(text:match("^%d+", pos), "invalid fraction")
			pos = pos + #digits
		end

		if text:sub(pos, pos):match("[eE]") then
			pos = pos + 1
			if text:sub(pos, pos):match("[+-]") then
				pos = pos + 1
			end

			local digits = assert(text:match("^%d+", pos), "invalid exponent")
			pos = pos + #digits
		end

		local value = assert(tonumber(text:sub(start, pos - 1)), "invalid number")
		assert(math.abs(value) ~= math.huge, "number overflow")
		return value
	end

	local value = parse(0)
	whitespace()
	assert(pos > #text, "trailing JSON data")
	return value
end

return M
