local skynet = require "skynet"
local protocol = require "team.protocol"

local test = {}

local function equal(actual, expected, message)
	if actual ~= expected then
		error(string.format("%s: expected %s, got %s", message, tostring(expected), tostring(actual)), 2)
	end
end

local function truthy(value, message)
	if not value then
		error(message, 2)
	end
end

local function find_delivery(deliveries, player_id, message_id)
	for _, delivery in ipairs(deliveries or {}) do
		if delivery.player_id == player_id and delivery.message_id == message_id then
			return delivery
		end
	end
	return nil
end

local function connect(team, player_id)
	local result = skynet.call(team, "lua", "connect", {
		player_id = player_id,
		display_name = "玩家" .. player_id,
		avatar_key = "avatar_" .. player_id,
	})
	truthy(result.sync, "connect must return a sync payload")
	return result
end

local function request(team, player_id, message_id, payload, request_id)
	payload.schema_version = protocol.SCHEMA_VERSION
	payload.type = protocol.REQUEST_TYPES[message_id]
	local result = skynet.call(team, "lua", "request", player_id, message_id, request_id, payload)
	truthy(result.message_id, "request must return a framed response")
	return result
end

function test.run()
	local team = skynet.uniqueservice("team")

	local first = connect(team, "alice")
	local second = connect(team, "bob")
	local third = connect(team, "carol")
	equal(first.sync.snapshot.team_id, "", "alice starts without a team")
	equal(second.sync.snapshot.team_id, "", "bob starts without a team")
	equal(third.sync.snapshot.team_id, "", "carol starts without a team")

	local invitation = request(team, "alice", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
		team_id = "",
		target_player_id = "bob",
	}, 101)
	equal(invitation.payload.ok, true, "invite succeeds")
	equal(invitation.payload.team_created, true, "invite creates alice's team")
	truthy(
		find_delivery(invitation.deliveries, "bob", protocol.MESSAGE.INVITATION),
		"bob receives invitation event"
	)

	local team_id = invitation.payload.snapshot.team_id
	local accepted = request(team, "bob", protocol.MESSAGE.RESPOND_INVITE_REQUEST, {
		invitation_id = invitation.payload.invitation_id,
		accept = true,
	}, 102)
	equal(accepted.payload.ok, true, "bob accepts invitation")
	equal(accepted.payload.outcome, "accepted", "accept outcome")
	equal(#accepted.payload.snapshot.members, 2, "team has two members")

	local synced = request(team, "bob", protocol.MESSAGE.SYNC_TEAM_REQUEST, {}, 103)
	equal(synced.request_id, 103, "sync response keeps request id")
	equal(synced.payload.type, "team_snapshot", "sync response type")
	equal(synced.payload.snapshot.team_id, team_id, "sync restores team")
	equal(#synced.payload.invitations, 0, "sync returns empty invitation list")

	local started = request(team, "alice", protocol.MESSAGE.START_BATTLE_REQUEST, {
		team_id = team_id,
		template_id = "battle_template",
	}, 104)
	equal(started.payload.ok, true, "battle starts through team battle provider")
	equal(started.payload.snapshot.membership_locked, true, "team locks for battle")
	local ready = find_delivery(started.deliveries, "bob", protocol.MESSAGE.BATTLE_READY)
	truthy(ready, "bob receives battle ready event")
	equal(ready.payload.entry.team_id, team_id, "battle entry belongs to team")
	equal(#ready.payload.entry.player_ids, 2, "battle entry preserves roster")

	local released = skynet.call(team, "lua", "release_battle", team_id)
	equal(released.ok, true, "battle release succeeds")
	truthy(
		find_delivery(released.deliveries, "alice", protocol.MESSAGE.TEAM_SNAPSHOT),
		"battle release pushes unlocked snapshot"
	)

	local disconnected = skynet.call(team, "lua", "disconnect", "bob")
	truthy(
		find_delivery(disconnected.deliveries, "alice", protocol.MESSAGE.TEAM_SNAPSHOT),
		"disconnect pushes offline state"
	)
	local reconnected = connect(team, "bob")
	equal(reconnected.sync.snapshot.team_id, team_id, "reconnect restores team membership")
	local bob_member
	for _, member in ipairs(reconnected.sync.snapshot.members) do
		if member.player_id == "bob" then
			bob_member = member
		end
	end
	truthy(bob_member and bob_member.online, "reconnect marks member online")

	local status = skynet.call(team, "lua", "status")
	equal(status.players, 3, "status player count")
	equal(status.online_players, 3, "status online count")
	equal(status.teams, 1, "status team count")
	equal(status.invitations, 0, "status invitation count")
end

return test
