-- All mutations are synchronous. The owning Skynet service persists a transaction
-- before publishing its ordered outbox; no RPC may yield inside a transaction.
local json = require "common.json"

local Model = {}
Model.__index = Model

local messages = {
	invalid_request = "请求参数无效",
	not_authenticated = "请先登录",
	not_in_team = "当前未组队",
	already_in_team = "玩家已有队伍",
	already_member = "目标已在本队",
	invalid_target = "目标玩家无效",
	target_offline = "目标玩家离线",
	team_not_found = "队伍不存在或已变更",
	team_full = "队伍已满",
	not_leader = "仅队长可执行此操作",
	invite_forbidden = "没有邀请权限",
	membership_locked = "队伍成员已锁定",
	activity_blocked = "当前活动不允许组队",
	invite_expired = "邀请已过期",
	invite_invalid = "邀请已失效",
	battle_blocked = "存在不能进入战斗的成员",
	battle_create_failed = "战斗服务暂不可用",
}

local requests = {
	[3] = "leave_team",
	[4] = "invite_team",
	[5] = "respond_invite",
	[6] = "kick_member",
	[7] = "sync_team",
	[8] = "start_battle",
}

local function nonempty(v)
	return type(v) == "string" and #v > 0 and #v <= 256
end

local function copy(v)
	return json.decode(json.encode(v))
end

local function body(kind, fields)
	fields = fields or {}
	fields.schema_version, fields.type = 3, kind
	return fields
end

function Model.new(state, options)
	return setmetatable({
		state = state or { format = 1, counter = 0, players = {}, teams = {}, invitations = {} },
		options = assert(options),
		events = {},
		jobs = {},
		now = os.time(),
	}, Model)
end

function Model:begin(now)
	self.events, self.jobs, self.now = {}, {}, now
end

function Model:id(prefix)
	self.state.counter = self.state.counter + 1
	return prefix .. tostring(self.state.counter)
end

function Model:player(id)
	return self.state.players[id]
end

function Model:team(p)
	return self.state.teams[p.team_id]
end

function Model:emit(id, message, data)
	if self:player(id).online then
		self.events[#self.events + 1] = { player_id = id, message = message, body = data }
	end
end

function Model:snapshot(id)
	local p = assert(self:player(id))
	local t = self:team(p)
	local snapshot = {
		state_version = p.version,
		team_id = p.team_id,
		leader_id = t and t.leader_id or "",
		members = json.array(),
		membership_locked = t and t.locked or false,
		lock_reason = t and t.lock_reason or "",
	}

	if t then
		for _, member_id in ipairs(t.members) do
			local member = self:player(member_id)
			local allowed = member.online and member.block_reason == ""
			snapshot.members[#snapshot.members + 1] = {
				player_id = member_id,
				display_name = member.display_name,
				avatar_key = member.avatar_key,
				online = member.online,
				can_enter_battle = allowed,
				battle_block_reason = not member.online and "成员离线" or member.block_reason,
			}
		end
	end

	return snapshot
end

function Model:changed(ids, removed)
	for _, id in ipairs(ids) do
		local p = self:player(id)
		assert(p.version < 9007199254740991, "state version exhausted")
		p.version = p.version + 1
	end

	for _, id in ipairs(ids) do
		if id ~= removed then
			self:emit(id, 101, body("team_snapshot", { snapshot = self:snapshot(id) }))
		end
	end
end

function Model:failure(code, id, fields)
	fields = fields or {}
	fields.ok, fields.error, fields.message = false, code, assert(messages[code])
	if id then
		fields.snapshot = self:snapshot(id)
	end

	return 255, body("error", fields)
end

function Model:success(message, kind, id, text, fields)
	fields = fields or {}
	fields.ok, fields.message, fields.snapshot = true, text, self:snapshot(id)
	return message, body(kind, fields)
end

function Model:invitation(invite)
	local p = self:player(invite.inviter_id)
	return {
		invitation_id = invite.id,
		team_id = invite.team_id,
		inviter = { player_id = p.id, display_name = p.display_name, avatar_key = p.avatar_key },
		expires_at = invite.expires_at,
		expires_in_sec = invite.expires_at - self.now,
	}
end

function Model:resolve(invite, outcome, code)
	self.state.invitations[invite.id] = nil

	local data = body("invite_resolved", {
		invitation_id = invite.id,
		outcome = outcome,
		message = code and messages[code] or ({
			accepted = "已加入队伍",
			declined = "邀请已拒绝",
			expired = "邀请已过期",
			invalidated = "邀请已失效",
		})[outcome],
		error = code,
	})

	self:emit(invite.inviter_id, 111, data)
	self:emit(invite.target_id, 111, data)
end

function Model:invalidate(predicate)
	local pending = {}
	for _, invite in pairs(self.state.invitations) do
		if predicate(invite) then
			pending[#pending + 1] = invite
		end
	end

	table.sort(pending, function(a, b)
		return a.id < b.id
	end)

	for _, invite in ipairs(pending) do
		self:resolve(invite, "invalidated", "invite_invalid")
	end
end

function Model:expire()
	local pending = {}
	for _, invite in pairs(self.state.invitations) do
		if invite.expires_at <= self.now then
			pending[#pending + 1] = invite
		end
	end

	table.sort(pending, function(a, b)
		return a.id < b.id
	end)

	for _, invite in ipairs(pending) do
		self:resolve(invite, "expired", "invite_expired")
	end
end

function Model:connect(profile)
	local p = self:player(profile.player_id)
	if not p then
		p = {
			id = profile.player_id,
			display_name = profile.display_name,
			avatar_key = profile.avatar_key,
			online = false,
			team_id = "",
			version = 0,
			block_reason = "",
		}

		self.state.players[p.id] = p
	end

	local changed = not p.online or p.display_name ~= profile.display_name or p.avatar_key ~= profile.avatar_key
	p.online, p.display_name, p.avatar_key = true, profile.display_name, profile.avatar_key
	if p.removed then
		-- Deliver the historical empty snapshot before any newer state updates.
		self:emit(p.id, 112, body("removed", p.removed))
		p.removed = nil
	end

	if changed then
		self:changed(self:team(p) and self:team(p).members or { p.id })
	end
end

function Model:disconnect(id)
	local p = self:player(id)
	if p and p.online then
		p.online = false
		self:changed(self:team(p) and self:team(p).members or { id })
	end
end

function Model:recover()
	-- Sessions do not survive process restarts; durable relationships and locks do.
	local affected = {}
	for id, p in pairs(self.state.players) do
		if p.online then
			p.online = false
			for _, member in ipairs(self:team(p) and self:team(p).members or { id }) do
				affected[member] = true
			end
		end
	end

	local ids = {}
	for id in pairs(affected) do
		ids[#ids + 1] = id
	end

	self:changed(ids)
	self:expire()
end

function Model:sync_team(id)
	self:expire()

	local invitations = json.array()
	for _, invite in pairs(self.state.invitations) do
		if invite.target_id == id then
			invitations[#invitations + 1] = self:invitation(invite)
		end
	end

	table.sort(invitations, function(a, b)
		return a.invitation_id < b.invitation_id
	end)

	local t = self:team(self:player(id))
	return 101,
		body("team_snapshot", {
			snapshot = self:snapshot(id),
			invitations = invitations,
			entry = t and t.entry and copy(t.entry) or json.null,
		})
end

function Model:current(id, team_id)
	local p = self:player(id)
	if p.team_id == "" then
		return nil, "not_in_team"
	end

	if p.team_id ~= team_id then
		return nil, "team_not_found"
	end

	local t = self:team(p)
	if t.locked then
		return nil, "membership_locked"
	end

	return t
end

function Model:invite_team(id, request)
	if type(request.team_id) ~= "string" or not nonempty(request.target_player_id) then
		return self:failure("invalid_request", id)
	end

	local p, target = self:player(id), self:player(request.target_player_id)
	if request.target_player_id == id or not target then
		return self:failure("invalid_target", id)
	end

	if p.team_id ~= request.team_id then
		return self:failure("team_not_found", id)
	end

	local t = self:team(p)
	if t and t.locked then
		return self:failure("membership_locked", id)
	end

	if p.block_reason ~= "" or target.block_reason ~= "" then
		return self:failure("activity_blocked", id)
	end

	if not target.online then
		return self:failure("target_offline", id)
	end

	if target.team_id ~= "" then
		return self:failure(t and target.team_id == t.id and "already_member" or "already_in_team", id)
	end

	if t and #t.members >= 5 then
		return self:failure("team_full", id)
	end

	if t and not self.options.can_invite(t, id) then
		return self:failure("invite_forbidden", id)
	end

	self:expire()

	local created = not t
	if not t then
		t = {
			id = self:id("team_"),
			leader_id = id,
			members = json.array({ id }),
			locked = false,
			lock_reason = "",
		}
		self.state.teams[t.id], p.team_id = t, t.id
		self:changed(t.members)
	end

	for _, invite in pairs(self.state.invitations) do
		if invite.team_id == t.id and invite.target_id == target.id then
			return self:success(104, "invite_team", id, "邀请已发送", {
				invitation_id = invite.id,
				team_created = created,
			})
		end
	end

	local invite = {
		id = self:id("invite_"),
		team_id = t.id,
		inviter_id = id,
		target_id = target.id,
		expires_at = self.now + self.options.invite_ttl,
	}

	self.state.invitations[invite.id] = invite

	self:emit(target.id, 110, body("invitation", self:invitation(invite)))
	return self:success(104, "invite_team", id, "邀请已发送", {
		invitation_id = invite.id,
		team_created = created,
	})
end

function Model:respond_invite(id, request)
	if not nonempty(request.invitation_id) or type(request.accept) ~= "boolean" then
		return self:failure("invalid_request", id)
	end

	local invite = self.state.invitations[request.invitation_id]
	if not invite or invite.target_id ~= id then
		return self:failure("invite_invalid", id)
	end

	if invite.expires_at <= self.now then
		self:resolve(invite, "expired", "invite_expired")
		return self:failure("invite_expired", id)
	end

	if not request.accept then
		self:resolve(invite, "declined")
		return self:success(105, "respond_invite", id, "已拒绝邀请", {
			invitation_id = invite.id,
			outcome = "declined",
		})
	end

	local p, inviter = self:player(id), self:player(invite.inviter_id)
	local t, code = self.state.teams[invite.team_id]
	if not t or inviter.team_id ~= invite.team_id then
		code = "invite_invalid"
	elseif t.locked then
		code = "membership_locked"
	elseif p.team_id ~= "" then
		code = "already_in_team"
	elseif p.block_reason ~= "" or inviter.block_reason ~= "" then
		code = "activity_blocked"
	elseif not inviter.online then
		code = "target_offline"
	elseif not self.options.can_invite(t, inviter.id) then
		code = "invite_forbidden"
	elseif #t.members >= 5 then
		code = "team_full"
	end

	if code then
		self:resolve(invite, "failed", code)
		return self:failure(code, id)
	end

	t.members[#t.members + 1], p.team_id = id, t.id

	self:changed(t.members)
	self:resolve(invite, "accepted")
	return self:success(105, "respond_invite", id, "已加入队伍", {
		invitation_id = invite.id,
		outcome = "accepted",
	})
end

function Model:remove(t, id, kicked)
	local affected = copy(t.members)
	for i, member in ipairs(t.members) do
		if member == id then
			table.remove(t.members, i)
			break
		end
	end

	local p = self:player(id)
	p.team_id = ""
	if #t.members == 0 then
		self.state.teams[t.id] = nil
	elseif t.leader_id == id then
		t.leader_id = self.options.successor(t, self.state.players)
	end

	self:invalidate(function(invite)
		return invite.inviter_id == id or (#t.members == 0 and invite.team_id == t.id)
	end)

	self:changed(affected, kicked and id or nil)

	if kicked then
		local data = { snapshot = self:snapshot(id), message = "你已被队长移出队伍" }
		if p.online then
			self:emit(id, 112, body("removed", data))
		else
			p.removed = data
		end
	end
end

function Model:leave_team(id, request)
	if not nonempty(request.team_id) then
		return self:failure("invalid_request", id)
	end

	local t, code = self:current(id, request.team_id)
	if not t then
		return self:failure(code, id)
	end

	self:remove(t, id, false)
	return self:success(103, "leave_team", id, "已离开队伍")
end

function Model:kick_member(id, request)
	if not nonempty(request.team_id) or not nonempty(request.target_player_id) then
		return self:failure("invalid_request", id)
	end

	local t, code = self:current(id, request.team_id)
	if not t then
		return self:failure(code, id)
	end

	if t.leader_id ~= id then
		return self:failure("not_leader", id)
	end

	local target = self:player(request.target_player_id)
	if not target or target.id == id or target.team_id ~= t.id then
		return self:failure("invalid_target", id)
	end

	self:remove(t, target.id, true)
	return self:success(106, "kick_member", id, "已移出队员")
end

function Model:start_battle(id, request)
	if not nonempty(request.team_id) or not nonempty(request.template_id) then
		return self:failure("invalid_request", id)
	end

	local t, code = self:current(id, request.team_id)
	if not t then
		return self:failure(code, id)
	end

	if t.leader_id ~= id then
		return self:failure("not_leader", id)
	end

	local blockers = json.array()
	for _, member in ipairs(t.members) do
		local p = self:player(member)
		if not p.online or p.block_reason ~= "" then
			blockers[#blockers + 1] = { player_id = member, reason = not p.online and "成员离线" or p.block_reason }
		end
	end

	if #blockers > 0 then
		return self:failure("battle_blocked", id, { blockers = blockers })
	end

	if not self.options.battle_available then
		return self:failure("battle_create_failed", id)
	end

	t.locked, t.lock_reason, t.battle_token = true, "战斗锁定", self:id("battle_request_")
	self:invalidate(function(invite)
		return invite.team_id == t.id
	end)

	self:changed(t.members)

	self.jobs[#self.jobs + 1] = {
		token = t.battle_token,
		team_id = t.id,
		player_ids = copy(t.members),
		template_id = request.template_id,
		params_json = json.encode(request.params or json.null),
	}

	return self:success(108, "start_battle", id, "正在创建队伍战斗")
end

function Model:battle_ready(team_id, token, entry)
	local t = self.state.teams[team_id]
	if not t or not t.locked or t.battle_token ~= token then
		return false
	end

	if type(entry) == "string" then
		entry = json.decode(entry)
	end

	if
		type(entry) ~= "table"
		or not nonempty(entry.battle_id)
		or not nonempty(entry.side_id)
		or entry.team_id ~= t.id
		or type(entry.player_ids) ~= "table"
		or #entry.player_ids ~= #t.members
	then
		return false
	end

	for i, id in ipairs(t.members) do
		if entry.player_ids[i] ~= id then
			return false
		end
	end

	entry.player_ids = json.array(entry.player_ids)
	entry = copy(entry)
	if t.entry then
		return json.encode(t.entry) == json.encode(entry)
	end

	t.entry = entry
	for _, id in ipairs(t.members) do
		self:emit(id, 109, body("battle_ready", { snapshot = self:snapshot(id), entry = copy(entry) }))
	end

	return true
end

function Model:battle_finished(team_id, token)
	local t = self.state.teams[team_id]
	if not t or not t.locked or t.battle_token ~= token then
		return false
	end

	t.locked, t.lock_reason, t.battle_token, t.entry = false, "", nil, nil
	self:changed(t.members)
	return true
end

function Model:set_activity(id, reason)
	local p = self:player(id)
	if not p or type(reason) ~= "string" then
		return false
	end

	if p.block_reason ~= reason then
		p.block_reason = reason
		self:changed(self:team(p) and self:team(p).members or { id })
	end

	return true
end

function Model:request(id, message, request)
	local p = self:player(id)
	if not p or not p.online then
		return self:failure("not_authenticated")
	end

	local kind = requests[message]
	if not kind or type(request) ~= "table" or request.schema_version ~= 3 or request.type ~= kind then
		return self:failure("invalid_request", id)
	end

	return self[kind](self, id, request)
end

return Model
