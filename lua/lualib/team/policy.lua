-- Implementation defaults from the shared functional document, section 2.1.
-- These remain configurable product choices, not additions to the wire contract.
return {
	can_invite = function()
		return true
	end,
	successor = function(team, players)
		for _, id in ipairs(team.members) do
			if players[id].online then
				return id
			end
		end

		return team.members[1]
	end,
}
