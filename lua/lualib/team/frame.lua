local json = require "team.json"

local frame = {}

frame.MAGIC = 0x50565A4E
frame.VERSION = 1
frame.HEADER_SIZE = 40
frame.MAX_PAYLOAD_SIZE = 1024 * 1024
frame.FLAG_REQUEST = 1
frame.FLAG_RESPONSE = 2
frame.FLAG_EVENT = 4
frame.FLAG_ERROR = 8
frame.FLAG_ACK = 16

local header_format = ">I4I2I2I2I2I2I2I8I4I4I4I4"

local function check_uint(value, bits, name)
	if math.type(value) ~= "integer" then
		error(string.format("%s must be an unsigned %d-bit integer", name, bits), 0)
	end
	local out_of_range = bits == 63 and value > math.maxinteger or bits < 63 and value >= (1 << bits)
	if value < 0 or out_of_range then
		error(string.format("%s must be an unsigned %d-bit integer", name, bits), 0)
	end
end

function frame.encode(packet)
	assert(type(packet) == "table", "frame packet must be a table")
	local payload = packet.payload
	if payload == nil then
		payload = ""
	elseif type(payload) == "table" then
		payload = json.encode(payload)
	end
	assert(type(payload) == "string", "frame payload must be a string or JSON-compatible table")
	assert(#payload <= frame.MAX_PAYLOAD_SIZE, "frame payload exceeds 1 MiB")

	local request_id = packet.request_id or 0
	local seq = packet.seq or 0
	check_uint(request_id, 63, "request_id")
	check_uint(seq, 32, "seq")
	check_uint(packet.service_id or 0, 16, "service_id")
	check_uint(packet.message_id or 0, 16, "message_id")
	check_uint(packet.flags or 0, 16, "flags")

	local header = string.pack(
		header_format,
		frame.MAGIC,
		frame.VERSION,
		frame.HEADER_SIZE,
		packet.service_id or 0,
		packet.message_id or 0,
		packet.flags or 0,
		0,
		request_id,
		seq,
		#payload,
		0,
		0
	)
	return header .. payload
end

local function decode_header(buffer)
	if #buffer < frame.HEADER_SIZE then
		return nil, nil, "need_more"
	end

	local fields = {
		string.unpack(header_format, buffer),
	}
	local packet = {
		magic = fields[1],
		version = fields[2],
		header_size = fields[3],
		service_id = fields[4],
		message_id = fields[5],
		flags = fields[6],
		request_id = fields[8],
		seq = fields[9],
		payload_size = fields[10],
	}

	if packet.magic ~= frame.MAGIC then
		return nil, nil, "invalid_magic"
	elseif packet.version ~= frame.VERSION then
		return nil, nil, "unsupported_version"
	elseif packet.header_size ~= frame.HEADER_SIZE then
		return nil, nil, "invalid_header_size"
	elseif fields[7] ~= 0 or fields[11] ~= 0 or fields[12] ~= 0 then
		return nil, nil, "non_zero_reserved"
	elseif packet.payload_size > frame.MAX_PAYLOAD_SIZE then
		return nil, nil, "payload_too_large"
	end
	return packet
end

function frame.try_decode(buffer)
	local packet, _, error_message = decode_header(buffer)
	if error_message == "need_more" then
		return nil, buffer, nil
	elseif error_message then
		return nil, nil, error_message
	end

	local packet_size = frame.HEADER_SIZE + packet.payload_size
	if #buffer < packet_size then
		return nil, buffer, nil
	end
	packet.payload = string.sub(buffer, frame.HEADER_SIZE + 1, packet_size)
	return packet, string.sub(buffer, packet_size + 1), nil
end

function frame.decode(buffer)
	local packet, rest, error_message = frame.try_decode(buffer)
	if error_message then
		return nil, error_message
	elseif packet == nil then
		return nil, "incomplete_frame"
	elseif rest ~= "" then
		return nil, "trailing_data"
	end
	return packet
end

function frame.decode_json(packet)
	assert(type(packet) == "table" and type(packet.payload) == "string")
	local ok, value = pcall(json.decode, packet.payload)
	if not ok then
		return nil, tostring(value)
	end
	if type(value) ~= "table" then
		return nil, "payload is not a JSON object"
	end
	return value
end

return frame
