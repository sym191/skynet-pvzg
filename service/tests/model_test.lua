package.path = "service/?.lua;" .. package.path
local json = require "common.json"
local Model = require "team.model"
local store = require "team.store"

local count = 0

local function check(condition, message)
	assert(condition, message)
	count = count + 1
end

local options = require "team.policy"
options.invite_ttl, options.battle_available = 60, true

local function fixture()
	local m = Model.new(nil, options)
	m:begin(1000)
	for _, id in ipairs({ "a", "b", "c", "d", "e", "f", "g" }) do
		m:connect({ player_id = id, display_name = id, avatar_key = "" })
	end

	return m
end

local kinds = {
	[3] = "leave_team",
	[4] = "invite_team",
	[5] = "respond_invite",
	[6] = "kick_member",
	[7] = "sync_team",
	[8] = "start_battle",
}

local function request(m, id, message, fields, now)
	m:begin(now or 1000)
	fields = fields or {}
	fields.schema_version, fields.type = 3, kinds[message]
	local number, response = m:request(id, message, fields)
	return response, number
end

local function invite(m, target, inviter)
	inviter = inviter or "a"
	local r = request(m, inviter, 4, { team_id = m:player(inviter).team_id, target_player_id = target })
	check(r.ok, json.encode(r))
	return r.invitation_id
end

local function join(m, target, inviter)
	local id = invite(m, target, inviter)
	local r = request(m, target, 5, { invitation_id = id, accept = true })
	check(r.ok and r.outcome == "accepted", "join failed")
	return r
end

local m = fixture()
local sync = request(m, "a", 7)
check(sync.snapshot.team_id == "" and json.encode(sync.snapshot.members) == "[]")
check(json.encode(sync.invitations) == "[]" and sync.entry == json.null)
check(request(m, "a", 4, { team_id = "", target_player_id = "a" }).error == "invalid_target")
check(m:player("a").team_id == "")

local invitation = invite(m, "b")
local team_id = m:player("a").team_id
check(#m.state.teams[team_id].members == 1)

local duplicate = request(m, "a", 4, { team_id = team_id, target_player_id = "b" })
check(duplicate.invitation_id == invitation and not duplicate.team_created and #m.events == 0)
check(request(m, "c", 5, { invitation_id = invitation, accept = true }).error == "invite_invalid")
check(request(m, "b", 5, { invitation_id = invitation, accept = "true" }).error == "invalid_request")
check(request(m, "b", 5, { invitation_id = invitation, accept = false }).outcome == "declined")
check(m.state.teams[team_id] ~= nil and #m.state.teams[team_id].members == 1)

invitation = invite(m, "b")
check(request(m, "b", 5, { invitation_id = invitation, accept = true }, 1060).error == "invite_expired")

join(m, "b")
join(m, "c", "b")
join(m, "d")
local invite_e, invite_f = invite(m, "e"), invite(m, "f")
check(request(m, "e", 5, { invitation_id = invite_e, accept = true }).ok)
check(request(m, "f", 5, { invitation_id = invite_f, accept = true }).error == "team_full")
check(#m.state.teams[team_id].members == 5)
check(request(m, "b", 6, { team_id = team_id, target_player_id = "c" }).error == "not_leader")
check(request(m, "b", 8, { team_id = team_id, template_id = "test" }).error == "not_leader")

local before = m:player("a").version
m:begin(1001)
m:disconnect("c")
check(m:player("a").version > before and m:player("c").team_id == team_id)
local blocked = request(m, "a", 8, { team_id = team_id, template_id = "test" })
check(blocked.error == "battle_blocked" and blocked.blockers[1].player_id == "c")
check(request(m, "a", 6, { team_id = team_id, target_player_id = "c" }).ok)
check(m:player("c").team_id == "" and m:player("c").removed.snapshot.team_id == "")

-- Persistence retains offline removal reasons and versions across a process restart.
local state_path = os.tmpname()
store.save(state_path, m.state)
m = Model.new(store.load(state_path), options)
os.remove(state_path)
m:begin(1002)
m:recover()
m:connect({ player_id = "c", display_name = "c", avatar_key = "" })
check(m.events[1].message == 112 and m.events[1].body.snapshot.team_id == "")
check(request(m, "c", 7).snapshot.team_id == "" and not m:player("c").removed)
for _, id in ipairs({ "a", "b", "d", "e", "f", "g" }) do
	m:connect({ player_id = id, display_name = id, avatar_key = "" })
end

-- Invitation and battle commits are serialized in both orders.
local old_invite = invite(m, "f")
local started = request(m, "a", 8, { team_id = team_id, template_id = "test" })
check(started.ok and started.snapshot.membership_locked and #m.jobs == 1)
local token = m.state.teams[team_id].battle_token
check(request(m, "f", 5, { invitation_id = old_invite, accept = true }).error == "invite_invalid")
for _, case in ipairs({
	{ "a", 4, { team_id = team_id, target_player_id = "g" } },
	{ "b", 3, { team_id = team_id } },
	{ "a", 6, { team_id = team_id, target_player_id = "b" } },
}) do
	check(request(m, table.unpack(case)).error == "membership_locked")
end

check(request(m, "a", 7).entry == json.null)
local entry = {
	battle_id = "battle_1",
	team_id = team_id,
	side_id = "side_1",
	player_ids = json.array({ "a", "b", "d", "e" }),
}

check(not m:battle_ready(team_id, "stale", entry))
check(m:battle_ready(team_id, token, entry))
check(request(m, "b", 7).entry.battle_id == "battle_1")
check(not m:battle_finished(team_id, "stale"))
m:disconnect("b")
check(m.state.teams[team_id].locked and #m.state.teams[team_id].members == 4)
check(m:battle_finished(team_id, token) and not m.state.teams[team_id].locked)
check(request(m, "a", 3, { team_id = team_id }).ok)
check(m.state.teams[team_id].leader_id == "d", "prefer online successor")

-- Cross-team versions never reset, and requests cannot nominate another actor.
before = m:player("a").version
join(m, "a", "g")
check(m:player("a").version > before)
check(
	request(m, "a", 6, { team_id = m:player("a").team_id, target_player_id = "g", player_id = "g" }).error
		== "not_leader"
)

m:set_activity("f", "单人战斗中")
check(request(m, "f", 4, { team_id = "", target_player_id = "c" }).error == "activity_blocked")
check(m:player("f").team_id == "")

m = fixture()
join(m, "b")
team_id = m:player("a").team_id
check(request(m, "b", 3, { team_id = team_id }).ok)
check(request(m, "a", 3, { team_id = team_id }).ok and not m.state.teams[team_id])

m = fixture()
local pending = invite(m, "b")
local other_pending = invite(m, "b", "c")
check(request(m, "b", 5, { invitation_id = pending, accept = true }).ok)
check(request(m, "b", 5, { invitation_id = other_pending, accept = true }).error == "already_in_team")
check(m:player("b").team_id == m:player("a").team_id)
team_id = m:player("a").team_id
check(request(m, "a", 8, { team_id = team_id, template_id = "test" }).ok)
check(#m.jobs[1].player_ids == 2 and m.jobs[1].player_ids[2] == "b")

m = fixture()
join(m, "b")
team_id = m:player("a").team_id
pending = invite(m, "c", "b")
check(request(m, "b", 3, { team_id = team_id }).ok)
check(not m.state.invitations[pending])
m:disconnect("a")
check(m.state.teams[team_id].leader_id == "a" and #m.state.teams[team_id].members == 1)
check(m.state.teams[team_id] ~= nil, "all-offline team retained")

-- JSON business payload edge cases.
for _, input in ipairs({
	'{"a":1,"a":2}',
	"[1,]",
	"01",
	"1.",
	"1e",
	"NaN",
	'"\\ud800"',
	'"\\udc00"',
	"{",
	"true false",
	'"\255"',
	"1e9999",
}) do
	check(not pcall(json.decode, input), "accepted invalid JSON: " .. input)
end

check(json.decode('"\\ud83d\\ude00"') == "😀")
check(json.encode(json.decode('{"x":[],"y":null}')) == '{"x":[],"y":null}')
print(string.format("PASS: %d model/JSON checks", count))
