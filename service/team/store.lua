local json = require "common.json"

local M = {}

function M.load(path)
	local file, message, code = io.open(path, "rb")
	if not file then
		assert(code == 2, message)
		return nil
	end

	local data = file:read("a")
	file:close()

	local state = json.decode(data)
	assert(
		state.format == 1
			and type(state.players) == "table"
			and type(state.teams) == "table"
			and type(state.invitations) == "table",
		"invalid team state file"
	)

	return state
end

function M.save(path, state)
	local data = json.encode(state)
	local file = assert(io.open(path .. ".tmp", "wb"))

	local ok, err = pcall(function()
		assert(file:write(data))
		assert(file:flush())
	end)

	local closed, close_error = file:close()
	assert(ok, err)
	assert(closed, close_error)

	assert(os.rename(path .. ".tmp", path))
end

return M
