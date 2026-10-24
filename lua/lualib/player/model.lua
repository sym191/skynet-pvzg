local M = {}

-- IDs use the same string representation as the Team protocol.
function M.valid_id(id)
	return type(id) == "string" and #id > 0 and #id <= 256 and not id:find("%c")
end

function M.new(id)
	assert(M.valid_id(id), "invalid player id")
	return {
		player_id = id,
		display_name = "玩家" .. id,
		avatar_key = "",
	}
end

return M
