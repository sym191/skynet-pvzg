local repo_root = arg[1] or "."
package.path = repo_root .. "/lua/lualib/?.lua;" .. package.path

local json = require "team.json"
local frame = require "team.frame"
local protocol = require "team.protocol"
local Model = require "team.model"

local failures = 0

local function fail(message)
	error(message, 2)
end

local function equal(actual, expected, message)
	if actual ~= expected then
		fail(string.format("%s: expected %s, got %s", message, tostring(expected), tostring(actual)))
	end
end

local function truthy(value, message)
	if not value then
		fail(message)
	end
end

local function run(name, test)
	io.write("RUN ", name, "\n")
	local ok, message = xpcall(test, debug.traceback)
	if ok then
		io.write("PASS ", name, "\n")
	else
		failures = failures + 1
		io.write("FAIL ", name, "\n", tostring(message), "\n")
	end
end

local function find_delivery(deliveries, player_id, message_id)
	for _, delivery in ipairs(deliveries) do
		if delivery.player_id == player_id and delivery.message_id == message_id then
			return delivery
		end
	end
	return nil
end

local function connect(model, player_id)
	local result = model:connect({
		player_id = player_id,
		display_name = player_id,
		avatar_key = "avatar_" .. player_id,
	})
	truthy(result.sync, "connect must return a sync payload")
	return result
end

local function request(model, player_id, message_id, payload, request_id)
	payload.schema_version = protocol.SCHEMA_VERSION
	payload.type = protocol.REQUEST_TYPES[message_id]
	return model:handle_request(player_id, message_id, request_id or 1, payload)
end

run("json round trip", function()
	local encoded = json.encode({
		name = "玩家",
		items = json.array({ 1, true, json.null, "x" }),
	})
	local decoded = json.decode(encoded)
	equal(decoded.name, "玩家", "unicode text")
	equal(decoded.items[1], 1, "integer array item")
	equal(decoded.items[2], true, "boolean array item")
	equal(decoded.items[3], json.null, "null array item")
	equal(json.encode({ empty = json.array() }), '{"empty":[]}', "empty array encoding")
	equal(json.decode('"\\ud83d\\ude03"'), "😃", "surrogate pair decoding")
	truthy(not pcall(json.decode, '{"a":1} trailing'), "trailing JSON must fail")
end)

run("frame encoding and stream parsing", function()
	local packet = {
		service_id = protocol.SERVICE_ID,
		message_id = protocol.MESSAGE.SYNC_TEAM_REQUEST,
		flags = protocol.FLAGS.REQUEST,
		request_id = 42,
		seq = 7,
		payload = {
			schema_version = 3,
			type = "sync_team",
		},
	}
	local bytes = frame.encode(packet)
	equal(#bytes > frame.HEADER_SIZE, true, "frame must contain a payload")

	local decoded, rest, error_message = frame.try_decode(bytes .. bytes)
	equal(error_message, nil, "first stream frame must decode")
	equal(decoded.request_id, 42, "request id")
	equal(decoded.message_id, protocol.MESSAGE.SYNC_TEAM_REQUEST, "message id")
	equal(#rest, #bytes, "second frame remains buffered")
	local second = frame.try_decode(rest)
	equal(second.request_id, 42, "second stream frame")

	local body = frame.decode_json(decoded)
	equal(body.type, "sync_team", "JSON body")

	local invalid = "\0" .. string.sub(bytes, 2)
	local _, _, invalid_error = frame.try_decode(invalid)
	equal(invalid_error, "invalid_magic", "magic validation")
end)

run("team lifecycle", function()
	local clock = 1000
	local model = Model.new({
		now = function()
			return clock
		end,
		invite_ttl_seconds = 30,
	})

	for index = 1, 6 do
		connect(model, "p" .. index)
	end

	local invite = request(model, "p1", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
		team_id = "",
		target_player_id = "p2",
	}, 1)
	equal(invite.payload.ok, true, "auto-created invite must succeed")
	equal(invite.payload.team_created, true, "first invite creates a team")
	equal(#invite.payload.snapshot.members, 1, "creator becomes sole member")
	equal(invite.payload.snapshot.leader_id, "p1", "creator becomes leader")
	truthy(find_delivery(invite.deliveries, "p2", protocol.MESSAGE.INVITATION), "target receives invitation")

	local invitation_id = invite.payload.invitation_id
	local team_id = invite.payload.snapshot.team_id
	local accepted = request(model, "p2", protocol.MESSAGE.RESPOND_INVITE_REQUEST, {
		invitation_id = invitation_id,
		accept = true,
	}, 2)
	equal(accepted.payload.ok, true, "accept invitation")
	equal(accepted.payload.outcome, "accepted", "accepted outcome")
	equal(#accepted.payload.snapshot.members, 2, "target joins team")
	local synced = request(model, "p2", protocol.MESSAGE.SYNC_TEAM_REQUEST, {}, 33)
	equal(synced.request_id, 33, "sync response keeps request id")
	equal(synced.payload.snapshot.team_id, team_id, "sync returns current team")

	local duplicate = request(model, "p1", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
		team_id = team_id,
		target_player_id = "p2",
	}, 3)
	equal(duplicate.payload.error, "already_member", "already member rejected")

	for index = 3, 5 do
		local invitation = request(model, "p1", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
			team_id = team_id,
			target_player_id = "p" .. index,
		}, index)
		equal(invitation.payload.ok, true, "invite member " .. index)
		local joined = request(model, "p" .. index, protocol.MESSAGE.RESPOND_INVITE_REQUEST, {
			invitation_id = invitation.payload.invitation_id,
			accept = true,
		}, index + 10)
		equal(joined.payload.ok, true, "member " .. index .. " joins")
	end

	local full = request(model, "p1", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
		team_id = team_id,
		target_player_id = "p6",
	}, 20)
	equal(full.payload.error, "team_full", "sixth member rejected")

	local not_leader = request(model, "p2", protocol.MESSAGE.KICK_MEMBER_REQUEST, {
		team_id = team_id,
		target_player_id = "p3",
	}, 21)
	equal(not_leader.payload.error, "not_leader", "ordinary member cannot kick")

	model:disconnect("p4")
	local blocked = request(model, "p1", protocol.MESSAGE.START_BATTLE_REQUEST, {
		team_id = team_id,
		template_id = "battle_template",
	}, 22)
	equal(blocked.payload.error, "battle_blocked", "offline member blocks battle")
	equal(blocked.payload.blockers[1].reason, "成员离线", "blocker reason")
	connect(model, "p4")

	local started = request(model, "p1", protocol.MESSAGE.START_BATTLE_REQUEST, {
		team_id = team_id,
		template_id = "battle_template",
	}, 23)
	equal(started.payload.ok, true, "leader starts battle")
	equal(started.payload.snapshot.membership_locked, true, "battle locks roster")
	truthy(started.battle_request, "battle request is emitted")

	local attached = model:attach_battle(team_id, {
		battle_id = "battle_000001",
		team_id = team_id,
		side_id = "side_team",
		player_ids = protocol.array({ "p1", "p2", "p3", "p4", "p5" }),
	})
	equal(attached.ok, true, "battle entry attaches")
	truthy(find_delivery(attached.deliveries, "p1", protocol.MESSAGE.BATTLE_READY), "battle ready event")

	local invite_id = request(model, "p1", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
		team_id = team_id,
		target_player_id = "p6",
	}, 24)
	equal(invite_id.payload.error, "membership_locked", "locked team rejects invitations")

	local leave_locked = request(model, "p2", protocol.MESSAGE.LEAVE_TEAM_REQUEST, {
		team_id = team_id,
	}, 25)
	equal(leave_locked.payload.error, "membership_locked", "leave is blocked while locked")

	local released = model:release_battle(team_id)
	equal(released.ok, true, "battle releases")
	truthy(find_delivery(released.deliveries, "p2", protocol.MESSAGE.TEAM_SNAPSHOT), "release pushes snapshot")

	model:disconnect("p2")
	local kicked = request(model, "p1", protocol.MESSAGE.KICK_MEMBER_REQUEST, {
		team_id = team_id,
		target_player_id = "p2",
	}, 26)
	equal(kicked.payload.ok, true, "leader kicks offline member")
	local reconnect = connect(model, "p2")
	truthy(find_delivery(reconnect.deliveries, "p2", protocol.MESSAGE.REMOVED), "offline kick is delivered on reconnect")
	equal(reconnect.sync.snapshot.team_id, "", "kicked player has empty snapshot")

	local promotion = request(model, "p1", protocol.MESSAGE.LEAVE_TEAM_REQUEST, {
		team_id = team_id,
	}, 27)
	equal(promotion.payload.ok, true, "leader leaves")
	equal(promotion.payload.snapshot.team_id, "", "leader gets empty snapshot")
	local p3_sync = model:sync("p3")
	equal(p3_sync.payload.snapshot.leader_id, "p3", "first online member succeeds leader")

	local p7 = connect(model, "p7")
	local p8 = connect(model, "p8")
	equal(p7.sync.snapshot.team_id, "", "p7 starts without team")
	equal(p8.sync.snapshot.team_id, "", "p8 starts without team")
	local expiring = request(model, "p7", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
		team_id = "",
		target_player_id = "p8",
	}, 28)
	clock = clock + 31
	local expired = request(model, "p8", protocol.MESSAGE.RESPOND_INVITE_REQUEST, {
		invitation_id = expiring.payload.invitation_id,
		accept = true,
	}, 29)
	equal(expired.payload.error, "invite_expired", "expired invitation rejected")
	equal(model:sync("p8").payload.snapshot.team_id, "", "expired invite does not join target")

	local declined_invite = request(model, "p7", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
		team_id = expiring.payload.snapshot.team_id,
		target_player_id = "p8",
	}, 30)
	local declined = request(model, "p8", protocol.MESSAGE.RESPOND_INVITE_REQUEST, {
		invitation_id = declined_invite.payload.invitation_id,
		accept = false,
	}, 31)
	equal(declined.payload.outcome, "declined", "decline is a successful response")

	local invalid_target = request(model, "p8", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
		team_id = "",
		target_player_id = "missing",
	}, 32)
	truthy(invalid_target.payload.team_created, "auto-created team remains after failed invite")
	equal(invalid_target.payload.error, "target_offline", "unknown target is offline")
end)

run("invitation contention", function()
	local model = Model.new({
		now = function()
			return 2000
		end,
	})
	for index = 1, 6 do
		connect(model, "c" .. index)
	end

	local first = request(model, "c1", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
		team_id = "",
		target_player_id = "c2",
	}, 201)
	request(model, "c2", protocol.MESSAGE.RESPOND_INVITE_REQUEST, {
		invitation_id = first.payload.invitation_id,
		accept = true,
	}, 202)
	local team_id = first.payload.snapshot.team_id
	for index = 3, 4 do
		local invitation = request(model, "c1", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
			team_id = team_id,
			target_player_id = "c" .. index,
		}, 200 + index)
		request(model, "c" .. index, protocol.MESSAGE.RESPOND_INVITE_REQUEST, {
			invitation_id = invitation.payload.invitation_id,
			accept = true,
		}, 210 + index)
	end

	local fifth = request(model, "c1", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
		team_id = team_id,
		target_player_id = "c5",
	}, 220)
	local sixth = request(model, "c1", protocol.MESSAGE.INVITE_TEAM_REQUEST, {
		team_id = team_id,
		target_player_id = "c6",
	}, 221)
	equal(fifth.payload.ok, true, "fifth player invitation is sent")
	equal(sixth.payload.ok, true, "sixth player invitation is sent before contention")

	local fifth_joined = request(model, "c5", protocol.MESSAGE.RESPOND_INVITE_REQUEST, {
		invitation_id = fifth.payload.invitation_id,
		accept = true,
	}, 222)
	equal(fifth_joined.payload.ok, true, "first acceptance wins final slot")
	local sixth_joined = request(model, "c6", protocol.MESSAGE.RESPOND_INVITE_REQUEST, {
		invitation_id = sixth.payload.invitation_id,
		accept = true,
	}, 223)
	equal(sixth_joined.payload.error, "team_full", "second acceptance cannot exceed capacity")
	equal(model:sync("c6").payload.snapshot.team_id, "", "losing target remains ungrouped")
end)

if failures > 0 then
	io.write(string.format("%d test(s) failed\n", failures))
	os.exit(1)
end
io.write("all team unit tests passed\n")
