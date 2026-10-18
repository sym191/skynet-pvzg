local json = {}

local array_mt = { __json_array = true }
local null = {}

json.null = null

function json.array(value)
	value = value or {}
	return setmetatable(value, array_mt)
end

local function is_array(value)
	if getmetatable(value) == array_mt then
		return true
	end

	local count = 0
	for key in pairs(value) do
		if math.type(key) ~= "integer" then
			return false
		end
		count = count + 1
	end

	for index = 1, count do
		if value[index] == nil then
			return false
		end
	end
	return count > 0
end

local function encode_string(value)
	return '"' .. value:gsub('[%c"\\]', function(char)
		local escapes = {
			['"'] = '\\"',
			["\\"] = "\\\\",
			["\b"] = "\\b",
			["\f"] = "\\f",
			["\n"] = "\\n",
			["\r"] = "\\r",
			["\t"] = "\\t",
		}
		return escapes[char] or string.format("\\u%04x", string.byte(char))
	end) .. '"'
end

local function encode_value(value, depth)
	if depth > 128 then
		error("json encode depth exceeded", 0)
	end

	local value_type = type(value)
	if value == null then
		return "null"
	elseif value_type == "nil" then
		return "null"
	elseif value_type == "boolean" then
		return value and "true" or "false"
	elseif value_type == "string" then
		return encode_string(value)
	elseif value_type == "number" then
		if math.type(value) == "integer" then
			return string.format("%d", value)
		end
		if value ~= value or value == math.huge or value == -math.huge then
			error("json cannot encode a non-finite number", 0)
		end
		return string.format("%.17g", value)
	elseif value_type ~= "table" then
		error("json cannot encode " .. value_type, 0)
	end

	local parts = {}
	if is_array(value) then
		for index = 1, #value do
			parts[index] = encode_value(value[index], depth + 1)
		end
		return "[" .. table.concat(parts, ",") .. "]"
	end

	local keys = {}
	for key in pairs(value) do
		if type(key) ~= "string" then
			error("json object keys must be strings", 0)
		end
		keys[#keys + 1] = key
	end
	table.sort(keys)
	for index, key in ipairs(keys) do
		parts[index] = encode_string(key) .. ":" .. encode_value(value[key], depth + 1)
	end
	return "{" .. table.concat(parts, ",") .. "}"
end

function json.encode(value)
	return encode_value(value, 0)
end

local function utf8_encode(codepoint)
	if codepoint < 0x80 then
		return string.char(codepoint)
	elseif codepoint < 0x800 then
		return string.char(
			0xC0 | (codepoint >> 6),
			0x80 | (codepoint & 0x3F)
		)
	elseif codepoint < 0x10000 then
		return string.char(
			0xE0 | (codepoint >> 12),
			0x80 | ((codepoint >> 6) & 0x3F),
			0x80 | (codepoint & 0x3F)
		)
	end
	return string.char(
		0xF0 | (codepoint >> 18),
		0x80 | ((codepoint >> 12) & 0x3F),
		0x80 | ((codepoint >> 6) & 0x3F),
		0x80 | (codepoint & 0x3F)
	)
end

local Parser = {}
Parser.__index = Parser

function Parser.new(source)
	return setmetatable({
		source = source,
		length = #source,
		position = 1,
		depth = 0,
	}, Parser)
end

function Parser:fail(message)
	error(string.format("json decode error at %d: %s", self.position, message), 0)
end

function Parser:skip_space()
	while self.position <= self.length do
		local byte = string.byte(self.source, self.position)
		if byte ~= 0x20 and byte ~= 0x09 and byte ~= 0x0A and byte ~= 0x0D then
			return
		end
		self.position = self.position + 1
	end
end

function Parser:parse_hex4()
	local text = string.sub(self.source, self.position, self.position + 3)
	if #text ~= 4 or not text:match("^%x%x%x%x$") then
		self:fail("invalid unicode escape")
	end
	self.position = self.position + 4
	return tonumber(text, 16)
end

function Parser:parse_string()
	if string.byte(self.source, self.position) ~= 0x22 then
		self:fail("expected string")
	end
	self.position = self.position + 1

	local parts = {}
	local chunk_start = self.position
	while self.position <= self.length do
		local byte = string.byte(self.source, self.position)
		if byte == 0x22 then
			parts[#parts + 1] = string.sub(self.source, chunk_start, self.position - 1)
			self.position = self.position + 1
			return table.concat(parts)
		elseif byte == 0x5C then
			parts[#parts + 1] = string.sub(self.source, chunk_start, self.position - 1)
			self.position = self.position + 1
			local escape = string.sub(self.source, self.position, self.position)
			local escapes = {
				['"'] = '"',
				["\\"] = "\\",
				["/"] = "/",
				["b"] = "\b",
				["f"] = "\f",
				["n"] = "\n",
				["r"] = "\r",
				["t"] = "\t",
			}
			if escapes[escape] then
				parts[#parts + 1] = escapes[escape]
				self.position = self.position + 1
			elseif escape == "u" then
				self.position = self.position + 1
				local codepoint = self:parse_hex4()
				if codepoint >= 0xD800 and codepoint <= 0xDBFF then
					if string.sub(self.source, self.position, self.position + 1) ~= "\\u" then
						self:fail("missing low surrogate")
					end
					self.position = self.position + 2
					local low = self:parse_hex4()
					if low < 0xDC00 or low > 0xDFFF then
						self:fail("invalid low surrogate")
					end
					codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00)
				elseif codepoint >= 0xDC00 and codepoint <= 0xDFFF then
					self:fail("unexpected low surrogate")
				end
				parts[#parts + 1] = utf8_encode(codepoint)
			else
				self:fail("invalid escape")
			end
			chunk_start = self.position
		elseif byte < 0x20 then
			self:fail("control character in string")
		else
			self.position = self.position + 1
		end
	end

	self:fail("unterminated string")
end

function Parser:parse_number()
	local start_position = self.position
	local text = self.source
	if string.sub(text, self.position, self.position) == "-" then
		self.position = self.position + 1
	end
	if string.sub(text, self.position, self.position) == "0" then
		self.position = self.position + 1
	elseif string.sub(text, self.position, self.position):match("[1-9]") then
		repeat
			self.position = self.position + 1
		until not string.sub(text, self.position, self.position):match("%d")
	else
		self:fail("invalid number")
	end
	if string.sub(text, self.position, self.position) == "." then
		self.position = self.position + 1
		local fraction_start = self.position
		repeat
			self.position = self.position + 1
		until not string.sub(text, self.position, self.position):match("%d")
		if self.position == fraction_start then
			self:fail("invalid fraction")
		end
	end
	local exponent = string.sub(text, self.position, self.position)
	if exponent == "e" or exponent == "E" then
		self.position = self.position + 1
		local sign = string.sub(text, self.position, self.position)
		if sign == "+" or sign == "-" then
			self.position = self.position + 1
		end
		local exponent_start = self.position
		repeat
			self.position = self.position + 1
		until not string.sub(text, self.position, self.position):match("%d")
		if self.position == exponent_start then
			self:fail("invalid exponent")
		end
	end
	local number_text = string.sub(text, start_position, self.position - 1)
	local value = tonumber(number_text)
	if value == nil or value ~= value or value == math.huge or value == -math.huge then
		self:fail("number out of range")
	end
	return value
end

function Parser:parse_array()
	self.depth = self.depth + 1
	if self.depth > 128 then
		self:fail("nesting too deep")
	end
	self.position = self.position + 1
	self:skip_space()
	local result = json.array()
	if string.sub(self.source, self.position, self.position) == "]" then
		self.position = self.position + 1
		self.depth = self.depth - 1
		return result
	end

	while true do
		result[#result + 1] = self:parse_value()
		self:skip_space()
		local char = string.sub(self.source, self.position, self.position)
		if char == "]" then
			self.position = self.position + 1
			self.depth = self.depth - 1
			return result
		elseif char ~= "," then
			self:fail("expected comma or closing bracket")
		end
		self.position = self.position + 1
		self:skip_space()
	end
end

function Parser:parse_object()
	self.depth = self.depth + 1
	if self.depth > 128 then
		self:fail("nesting too deep")
	end
	self.position = self.position + 1
	self:skip_space()
	local result = {}
	if string.sub(self.source, self.position, self.position) == "}" then
		self.position = self.position + 1
		self.depth = self.depth - 1
		return result
	end

	while true do
		local key = self:parse_string()
		self:skip_space()
		if string.sub(self.source, self.position, self.position) ~= ":" then
			self:fail("expected colon")
		end
		self.position = self.position + 1
		self:skip_space()
		result[key] = self:parse_value()
		self:skip_space()
		local char = string.sub(self.source, self.position, self.position)
		if char == "}" then
			self.position = self.position + 1
			self.depth = self.depth - 1
			return result
		elseif char ~= "," then
			self:fail("expected comma or closing brace")
		end
		self.position = self.position + 1
		self:skip_space()
	end
end

function Parser:parse_value()
	self:skip_space()
	if self.position > self.length then
		self:fail("unexpected end of input")
	end

	local byte = string.byte(self.source, self.position)
	if byte == 0x7B then
		return self:parse_object()
	elseif byte == 0x5B then
		return self:parse_array()
	elseif byte == 0x22 then
		return self:parse_string()
	elseif byte == 0x74 and string.sub(self.source, self.position, self.position + 3) == "true" then
		self.position = self.position + 4
		return true
	elseif byte == 0x66 and string.sub(self.source, self.position, self.position + 4) == "false" then
		self.position = self.position + 5
		return false
	elseif byte == 0x6E and string.sub(self.source, self.position, self.position + 3) == "null" then
		self.position = self.position + 4
		return null
	end
	return self:parse_number()
end

function json.decode(source)
	if type(source) ~= "string" then
		error("json decode expects a string", 0)
	end
	local parser = Parser.new(source)
	local result = parser:parse_value()
	parser:skip_space()
	if parser.position <= parser.length then
		parser:fail("trailing data")
	end
	return result
end

return json
