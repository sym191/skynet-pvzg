local protocol = require "team.protocol"

local Model = {}
Model.__index = Model

local function non_empty_string(value)
	return type(value) == "string" and value ~= ""
end

local function contains(values, target)
	for _, value in ipairs(values) do
		if value == target then
			return true
		end
	end
	return false
end

local function remove_value(values, target)
	for index, value in ipairs(values) do
		if value == target then
			table.remove(values, index)
			return true
		end
	end
	return false
end

function Model.new(options)
	options = options or {}
	return setmetatable({
		now = options.now or os.time,
		invite_ttl_seconds = options.invite_ttl_seconds or 60,
		max_members = options.max_members or 5,
		leader_succession = options.leader_succession or "online_first",
		invite_permission = options.invite_permission or "all",
		players = {},
		teams = {},
		invitations = {},
		invitation_order = {},
		player_invitations = {},
		pending_removed = {},
		next_team_id = 0,
		next_invitation_id = 0,
		next_battle_id = 0,
	}, Model)
end

function Model:_next_id(kind)
	if kind == "team" then
		self.next_team_id = self.next_team_id + 1
		return string.format("team_%06d", self.next_team_id)
	elseif kind == "invitation" then
		self.next_invitation_id = self.next_invitation_id + 1
		return string.format("invite_%06d", self.next_invitation_id)
	end
	error("unknown id kind " .. tostring(kind), 0)
end

function Model:_player(player_id)
	local player = self.players[player_id]
	if player then
		return player
	end
	player = {
		player_id = player_id,
		display_name = player_id,
		avatar_key = "",
		online = false,
		can_enter_battle = true,
		battle_block_reason = "",
		team_id = "",
		state_version = 0,
	}
	self.players[player_id] = player
	return player
end

function Model:_bump(player_ids)
	local seen = {}
	for _, player_id in ipairs(player_ids) do
		if not seen[player_id] then
			seen[player_id] = true
			local player = self:_player(player_id)
			player.state_version = player.state_version + 1
		end
	end
end

function Model:_bump_team(team)
	self:_bump(team.member_ids)
end

function Model:_snapshot_of(player)
	local team = self.teams[player.team_id]
	if not team then
		return protocol.empty_snapshot(player.state_version)
	end

	local members = {}
	for _, player_id in ipairs(team.member_ids) do
		members[#members + 1] = protocol.member(self:_player(player_id))
	end
	return protocol.snapshot(
		player.state_version,
		team.team_id,
		team.leader_id,
		members,
		team.membership_locked,
		team.lock_reason
	)
end

function Model:_deliver(deliveries, player_id, message_id, payload, flags)
	local player = self.players[player_id]
	if player and player.online then
		deliveries[#deliveries + 1] = {
			player_id = player_id,
			message_id = message_id,
			flags = flags or protocol.FLAGS.EVENT,
			payload = payload,
		}
	end
end

function Model:_push_team(team, deliveries, excluded_player_id)
	for _, player_id in ipairs(team.member_ids) do
		if player_id ~= excluded_player_id then
			local player = self:_player(player_id)
			self:_deliver(
				deliveries,
				player_id,
				protocol.MESSAGE.TEAM_SNAPSHOT,
				protocol.snapshot_event(self:_snapshot_of(player))
			)
		end
	end
end

function Model:_invitation_summary(invitation)
	local inviter = self:_player(invitation.inviter_id)
	local current_time = self.now()
	invitation.expires_in_sec = math.max(0, invitation.expires_at - current_time)
	return protocol.invitation_summary(invitation, inviter)
end

function Model:_remove_invitation(invitation_id)
	local invitation = self.invitations[invitation_id]
	if not invitation then
		return nil
	end
	self.invitations[invitation_id] = nil
	local target_invitations = self.player_invitations[invitation.target_id]
	if target_invitations then
		target_invitations[invitation_id] = nil
		if next(target_invitations) == nil then
			self.player_invitations[invitation.target_id] = nil
		end
	end
	return invitation
end

function Model:_resolve_invitation(invitation, outcome, message, deliveries, error_code)
	local removed = self:_remove_invitation(invitation.invitation_id)
	if not removed then
		return
	end
	local event = protocol.invite_resolved_event(
		removed.invitation_id,
		outcome,
		message,
		error_code
	)
	self:_deliver(deliveries, removed.inviter_id, protocol.MESSAGE.INVITE_RESOLVED, event)
	if removed.target_id ~= removed.inviter_id then
		self:_deliver(deliveries, removed.target_id, protocol.MESSAGE.INVITE_RESOLVED, event)
	end
end

function Model:_cleanup_expired(deliveries)
	local current_time = self.now()
	for _, invitation in ipairs(self.invitation_order) do
		if self.invitations[invitation.invitation_id] == invitation
			and invitation.expires_at <= current_time then
			self:_resolve_invitation(
				invitation,
				"expired",
				"邀请已过期",
				deliveries,
				"invite_expired"
			)
		end
	end
end

function Model:_invalidate_team_invitations(team_id, deliveries, message)
	for _, invitation in ipairs(self.invitation_order) do
		if self.invitations[invitation.invitation_id] == invitation
			and invitation.team_id == team_id then
			self:_resolve_invitation(
				invitation,
				"invalidated",
				message or "队伍状态变化，邀请已失效",
				deliveries,
				"invite_invalid"
			)
		end
	end
end

function Model:_invalidate_inviter_invitations(inviter_id, deliveries)
	for _, invitation in ipairs(self.invitation_order) do
		if self.invitations[invitation.invitation_id] == invitation
			and invitation.inviter_id == inviter_id then
			self:_resolve_invitation(
				invitation,
				"invalidated",
				"邀请者已离开队伍，邀请已失效",
				deliveries,
				"invite_invalid"
			)
		end
	end
end

function Model:_invalidate_target_invitations(target_id, except_invitation_id, deliveries)
	local invitation_ids = {}
	for invitation_id in pairs(self.player_invitations[target_id] or {}) do
		invitation_ids[#invitation_ids + 1] = invitation_id
	end
	for _, invitation_id in ipairs(invitation_ids) do
		if invitation_id ~= except_invitation_id then
			local invitation = self.invitations[invitation_id]
			if invitation then
				self:_resolve_invitation(
					invitation,
					"invalidated",
					"玩家已加入其他队伍，邀请已失效",
					deliveries,
					"invite_invalid"
				)
			end
		end
	end
end

function Model:_active_invitations(player_id)
	local result = {}
	for invitation_id in pairs(self.player_invitations[player_id] or {}) do
		local invitation = self.invitations[invitation_id]
		if invitation then
			result[#result + 1] = self:_invitation_summary(invitation)
		end
	end
	table.sort(result, function(left, right)
		return left.invitation_id < right.invitation_id
	end)
	return result
end

function Model:_error(code, message, request_id, deliveries, extras)
	return {
		request_id = request_id,
		message_id = protocol.MESSAGE.ERROR,
		flags = protocol.FLAGS.RESPONSE | protocol.FLAGS.ERROR,
		payload = protocol.error(code, message, extras),
		deliveries = deliveries or {},
	}
end

function Model:_success(message_type, message, snapshot, request_id, deliveries, extras)
	return {
		request_id = request_id,
		message_id = protocol.RESPONSE_MESSAGES[message_type],
		flags = protocol.FLAGS.RESPONSE,
		payload = protocol.success(message_type, message, snapshot, extras),
		deliveries = deliveries or {},
	}
end

function Model:_create_team(player)
	local team = {
		team_id = self:_next_id("team"),
		leader_id = player.player_id,
		member_ids = { player.player_id },
		membership_locked = false,
		lock_reason = "",
		battle_entry = nil,
	}
	self.teams[team.team_id] = team
	player.team_id = team.team_id
	self:_bump_team(team)
	return team
end

function Model:connect(principal)
	local deliveries = {}
	if type(principal) ~= "table" or not non_empty_string(principal.player_id) then
		return {
			error = "invalid_request",
			message = "连接身份无效",
			deliveries = deliveries,
		}
	end

	local player = self:_player(principal.player_id)
	local changed = not player.online
	local display_name = principal.display_name or player.display_name
	if type(display_name) ~= "string" or display_name == "" then
		display_name = player.player_id
	end
	local avatar_key = principal.avatar_key or player.avatar_key or ""
	if type(avatar_key) ~= "string" then
		avatar_key = ""
	end
	if player.display_name ~= display_name or player.avatar_key ~= avatar_key then
		changed = true
		player.display_name = display_name
		player.avatar_key = avatar_key
	end
	player.online = true

	if changed then
		if player.team_id ~= "" then
			local team = self.teams[player.team_id]
			if team then
				self:_bump_team(team)
			else
				player.team_id = ""
				self:_bump({ player.player_id })
			end
		else
			self:_bump({ player.player_id })
		end
	end

	self:_cleanup_expired(deliveries)

	local removed = self.pending_removed[player.player_id]
	if removed then
		self.pending_removed[player.player_id] = nil
		removed.snapshot.state_version = player.state_version
		self:_deliver(
			deliveries,
			player.player_id,
			protocol.MESSAGE.REMOVED,
			protocol.removed_event(removed.snapshot, removed.message)
		)
	end

	local team = self.teams[player.team_id]
	if team then
		self:_push_team(team, deliveries, player.player_id)
	end

	local entry = team and team.battle_entry or nil
	return {
		player_id = player.player_id,
		sync = protocol.sync_response(
			self:_snapshot_of(player),
			self:_active_invitations(player.player_id),
			entry
		),
		deliveries = deliveries,
	}
end

function Model:disconnect(player_id)
	local deliveries = {}
	local player = self.players[player_id]
	if not player or not player.online then
		return { deliveries = deliveries }
	end
	player.online = false
	self:_cleanup_expired(deliveries)
	if player.team_id ~= "" then
		local team = self.teams[player.team_id]
		if team then
			self:_bump_team(team)
			self:_push_team(team, deliveries, player_id)
		end
	end
	return {
		player_id = player_id,
		deliveries = deliveries,
	}
end

function Model:sync(player_id, request_id)
	local deliveries = {}
	self:_cleanup_expired(deliveries)
	local player = self.players[player_id]
	if not player or not player.online then
		return self:_error("not_authenticated", "连接未认证", nil, deliveries)
	end
	local team = self.teams[player.team_id]
	local entry = team and team.battle_entry or nil
	local result = self:_success(
		"sync_team",
		"当前队伍状态",
		self:_snapshot_of(player),
		request_id,
		deliveries
	)
	result.payload = protocol.sync_response(
		self:_snapshot_of(player),
		self:_active_invitations(player_id),
		entry
	)
	result.message_id = protocol.MESSAGE.TEAM_SNAPSHOT
	result.flags = protocol.FLAGS.RESPONSE
	return result
end

function Model:set_battle_eligibility(player_id, can_enter_battle, reason)
	if not non_empty_string(player_id) or type(can_enter_battle) ~= "boolean" then
		return { ok = false, error = "invalid_request" }
	end
	local player = self.players[player_id]
	if not player then
		return { ok = false, error = "invalid_target" }
	end
	reason = reason or ""
	if type(reason) ~= "string" then
		reason = ""
	end
	if player.can_enter_battle == can_enter_battle
		and player.battle_block_reason == reason then
		return { ok = true, deliveries = {} }
	end

	player.can_enter_battle = can_enter_battle
	player.battle_block_reason = reason
	local deliveries = {}
	if player.team_id ~= "" then
		local team = self.teams[player.team_id]
		if team then
			self:_bump_team(team)
			self:_push_team(team, deliveries)
		end
	end
	return { ok = true, deliveries = deliveries }
end

function Model:_invite(player, payload, request_id)
	local deliveries = {}
	self:_cleanup_expired(deliveries)

	local target_player_id = payload.target_player_id
	if not non_empty_string(target_player_id) or target_player_id == player.player_id then
		return self:_error("invalid_target", "邀请目标无效", request_id, deliveries)
	end

	local team
	local team_created = false
	if player.team_id == "" then
		if non_empty_string(payload.team_id) then
			return self:_error("not_in_team", "当前未组队", request_id, deliveries)
		end
		team = self:_create_team(player)
		team_created = true
		self:_deliver(
			deliveries,
			player.player_id,
			protocol.MESSAGE.TEAM_SNAPSHOT,
			protocol.snapshot_event(self:_snapshot_of(player))
		)
	else
		if non_empty_string(payload.team_id) and payload.team_id ~= player.team_id then
			return self:_error(
				"not_in_team",
				"队伍身份不匹配",
				request_id,
				deliveries,
				{ snapshot = self:_snapshot_of(player) }
			)
		end
		team = self.teams[player.team_id]
		if not team then
			player.team_id = ""
			return self:_error("team_not_found", "队伍不存在", request_id, deliveries)
		end
	end

	local function failed(code, message, extras)
		if team_created then
			extras = extras or {}
			extras.team_created = true
			extras.snapshot = self:_snapshot_of(player)
		end
		return self:_error(code, message, request_id, deliveries, extras)
	end

	if team.membership_locked then
		return failed("membership_locked", "队伍成员关系已锁定")
	end
	if #team.member_ids >= self.max_members then
		return failed("team_full", "队伍人数已满")
	end
	if self.invite_permission == "leader" and team.leader_id ~= player.player_id then
		return failed("invite_forbidden", "仅队长可以邀请玩家")
	end
	if not player.can_enter_battle then
		return failed("activity_blocked", "当前活动状态不允许组队")
	end

	local target = self.players[target_player_id]
	if not target or not target.online then
		return failed("target_offline", "目标玩家已离线")
	end
	if target.team_id ~= "" then
		if target.team_id == team.team_id then
			return failed("already_member", "目标玩家已在队伍中")
		end
		return failed("already_in_team", "目标玩家已有队伍")
	end
	if not target.can_enter_battle then
		return failed("activity_blocked", "目标玩家当前活动状态不允许组队")
	end

	local existing
	for invitation_id in pairs(self.player_invitations[target_player_id] or {}) do
		local invitation = self.invitations[invitation_id]
		if invitation and invitation.team_id == team.team_id then
			existing = invitation
			break
		end
	end

	local invitation = existing
	if not invitation then
		invitation = {
			invitation_id = self:_next_id("invitation"),
			team_id = team.team_id,
			inviter_id = player.player_id,
			target_id = target_player_id,
			expires_at = self.now() + self.invite_ttl_seconds,
		}
		self.invitations[invitation.invitation_id] = invitation
		self.invitation_order[#self.invitation_order + 1] = invitation
		self.player_invitations[target_player_id] = self.player_invitations[target_player_id] or {}
		self.player_invitations[target_player_id][invitation.invitation_id] = true
	end
	invitation.expires_in_sec = math.max(0, invitation.expires_at - self.now())

	self:_deliver(
		deliveries,
		target_player_id,
		protocol.MESSAGE.INVITATION,
		protocol.invitation_event(invitation, player)
	)
	return self:_success(
		"invite_team",
		"邀请已发送",
		self:_snapshot_of(player),
		request_id,
		deliveries,
		{
			invitation_id = invitation.invitation_id,
			team_created = team_created,
		}
	)
end

function Model:_respond_invite(player, payload, request_id)
	local deliveries = {}
	local invitation_id = payload.invitation_id
	if not non_empty_string(invitation_id) or type(payload.accept) ~= "boolean" then
		return self:_error("invalid_request", "邀请响应参数无效", request_id, deliveries)
	end

	local invitation = self.invitations[invitation_id]
	if not invitation then
		self:_cleanup_expired(deliveries)
		return self:_error("invite_invalid", "邀请不存在或已失效", request_id, deliveries)
	end
	if invitation.expires_at <= self.now() then
		self:_resolve_invitation(
			invitation,
			"expired",
			"邀请已过期",
			deliveries,
			"invite_expired"
		)
		return self:_error("invite_expired", "邀请已过期", request_id, deliveries)
	end
	if invitation.target_id ~= player.player_id then
		return self:_error("invite_invalid", "邀请不属于当前玩家", request_id, deliveries)
	end

	self:_cleanup_expired(deliveries)

	if not payload.accept then
		self:_resolve_invitation(invitation, "declined", "已拒绝邀请", deliveries)
		return self:_success(
			"respond_invite",
			"已拒绝邀请",
			self:_snapshot_of(player),
			request_id,
			deliveries,
			{
				invitation_id = invitation_id,
				outcome = "declined",
			}
		)
	end

	local team = self.teams[invitation.team_id]
	if not team then
		self:_resolve_invitation(invitation, "failed", "队伍已不存在", deliveries, "team_not_found")
		return self:_error("team_not_found", "队伍已不存在", request_id, deliveries)
	end
	if team.membership_locked then
		return self:_error(
			"membership_locked",
			"队伍成员关系已锁定",
			request_id,
			deliveries,
			{ snapshot = self:_snapshot_of(player) }
		)
	end
	if player.team_id ~= "" then
		return self:_error(
			"already_in_team",
			"当前玩家已有队伍",
			request_id,
			deliveries,
			{ snapshot = self:_snapshot_of(player) }
		)
	end
	if not player.online then
		return self:_error("not_authenticated", "连接未认证", request_id, deliveries)
	end
	if not player.can_enter_battle then
		return self:_error(
			"activity_blocked",
			"当前活动状态不允许加入队伍",
			request_id,
			deliveries,
			{ snapshot = self:_snapshot_of(player) }
		)
	end
	if not contains(team.member_ids, invitation.inviter_id) then
		self:_resolve_invitation(invitation, "invalidated", "邀请者已离开队伍", deliveries)
		return self:_error("invite_invalid", "邀请者已离开队伍", request_id, deliveries)
	end
	local inviter = self:_player(invitation.inviter_id)
	if not inviter.online or not inviter.can_enter_battle then
		self:_resolve_invitation(invitation, "invalidated", "邀请者当前不可组队", deliveries)
		return self:_error("activity_blocked", "邀请者当前不可组队", request_id, deliveries)
	end
	if #team.member_ids >= self.max_members then
		return self:_error(
			"team_full",
			"队伍人数已满",
			request_id,
			deliveries,
			{ snapshot = self:_snapshot_of(player) }
		)
	end

	self:_remove_invitation(invitation_id)
	self:_invalidate_target_invitations(player.player_id, invitation_id, deliveries)
	team.member_ids[#team.member_ids + 1] = player.player_id
	player.team_id = team.team_id
	self:_bump_team(team)
	self:_push_team(team, deliveries)

	local resolved = protocol.invite_resolved_event(
		invitation_id,
		"accepted",
		"玩家已加入队伍"
	)
	self:_deliver(deliveries, invitation.inviter_id, protocol.MESSAGE.INVITE_RESOLVED, resolved)
	self:_deliver(deliveries, player.player_id, protocol.MESSAGE.INVITE_RESOLVED, resolved)

	return self:_success(
		"respond_invite",
		"已加入队伍",
		self:_snapshot_of(player),
		request_id,
		deliveries,
		{
			invitation_id = invitation_id,
			outcome = "accepted",
		}
	)
end

function Model:_leave_team(player, payload, request_id)
	local deliveries = {}
	if not non_empty_string(payload.team_id) or payload.team_id ~= player.team_id then
		return self:_error(
			"not_in_team",
			"当前不在该队伍中",
			request_id,
			deliveries,
			{ snapshot = self:_snapshot_of(player) }
		)
	end
	local team = self.teams[payload.team_id]
	if not team then
		player.team_id = ""
		return self:_error("team_not_found", "队伍不存在", request_id, deliveries)
	end
	if team.membership_locked then
		return self:_error(
			"membership_locked",
			"队伍成员关系已锁定",
			request_id,
			deliveries,
			{ snapshot = self:_snapshot_of(player) }
		)
	end

	remove_value(team.member_ids, player.player_id)
	player.team_id = ""
	self:_bump({ player.player_id })
	local empty_snapshot = self:_snapshot_of(player)
	self:_invalidate_inviter_invitations(player.player_id, deliveries)

	if #team.member_ids == 0 then
		self.teams[team.team_id] = nil
		self:_invalidate_team_invitations(team.team_id, deliveries)
	else
		if team.leader_id == player.player_id then
			local successor = team.member_ids[1]
			if self.leader_succession == "online_first" then
				for _, member_id in ipairs(team.member_ids) do
					if self:_player(member_id).online then
						successor = member_id
						break
					end
				end
			end
			team.leader_id = successor
		end
		self:_bump_team(team)
		self:_push_team(team, deliveries)
	end

	return self:_success(
		"leave_team",
		"已离开队伍",
		empty_snapshot,
		request_id,
		deliveries
	)
end

function Model:_kick_member(player, payload, request_id)
	local deliveries = {}
	if not non_empty_string(payload.team_id)
		or not non_empty_string(payload.target_player_id) then
		return self:_error("invalid_request", "踢人参数无效", request_id, deliveries)
	end
	local team = self.teams[payload.team_id]
	if not team then
		return self:_error("team_not_found", "队伍不存在", request_id, deliveries)
	end
	if team.leader_id ~= player.player_id then
		return self:_error(
			"not_leader",
			"仅队长可以踢出成员",
			request_id,
			deliveries,
			{ snapshot = self:_snapshot_of(player) }
		)
	end
	if payload.target_player_id == player.player_id then
		return self:_error("invalid_target", "队长不能踢出自己", request_id, deliveries)
	end
	if not contains(team.member_ids, payload.target_player_id) then
		return self:_error("invalid_target", "目标玩家不在队伍中", request_id, deliveries)
	end
	if team.membership_locked then
		return self:_error(
			"membership_locked",
			"队伍成员关系已锁定",
			request_id,
			deliveries,
			{ snapshot = self:_snapshot_of(player) }
		)
	end

	local target = self:_player(payload.target_player_id)
	remove_value(team.member_ids, target.player_id)
	target.team_id = ""
	self:_bump({ target.player_id })
	local empty_snapshot = self:_snapshot_of(target)
	self:_bump_team(team)
	self:_invalidate_inviter_invitations(target.player_id, deliveries)

	local message = "你已被队长移出队伍"
	if target.online then
		self:_deliver(
			deliveries,
			target.player_id,
			protocol.MESSAGE.REMOVED,
			protocol.removed_event(empty_snapshot, message)
		)
	else
		self.pending_removed[target.player_id] = {
			snapshot = empty_snapshot,
			message = message,
		}
	end
	self:_push_team(team, deliveries)

	return self:_success(
		"kick_member",
		"已移出队员",
		self:_snapshot_of(player),
		request_id,
		deliveries
	)
end

function Model:_start_battle(player, payload, request_id)
	local deliveries = {}
	if not non_empty_string(payload.team_id)
		or not non_empty_string(payload.template_id) then
		return self:_error("invalid_request", "开战参数无效", request_id, deliveries)
	end
	local team = self.teams[payload.team_id]
	if not team then
		return self:_error("team_not_found", "队伍不存在", request_id, deliveries)
	end
	if team.leader_id ~= player.player_id then
		return self:_error(
			"not_leader",
			"仅队长可以发起队伍战斗",
			request_id,
			deliveries,
			{ snapshot = self:_snapshot_of(player) }
		)
	end
	if team.membership_locked then
		return self:_error(
			"membership_locked",
			"队伍成员关系已锁定",
			request_id,
			deliveries,
			{ snapshot = self:_snapshot_of(player) }
		)
	end

	local blockers = protocol.array()
	for _, member_id in ipairs(team.member_ids) do
		local member = self:_player(member_id)
		if not member.online then
			blockers[#blockers + 1] = {
				player_id = member_id,
				reason = "成员离线",
			}
		elseif not member.can_enter_battle then
			blockers[#blockers + 1] = {
				player_id = member_id,
				reason = member.battle_block_reason ~= "" and member.battle_block_reason
					or "成员当前不可进入战斗",
			}
		end
	end
	if #blockers > 0 then
		return self:_error(
			"battle_blocked",
			"存在无法进入战斗的成员",
			request_id,
			deliveries,
			{
				snapshot = self:_snapshot_of(player),
				blockers = blockers,
			}
		)
	end

	team.membership_locked = true
	team.lock_reason = "战斗锁定"
	self:_invalidate_team_invitations(team.team_id, deliveries)
	self:_bump_team(team)
	self:_push_team(team, deliveries)

	local player_ids = protocol.array()
	for _, member_id in ipairs(team.member_ids) do
		player_ids[#player_ids + 1] = member_id
	end

	local result = self:_success(
		"start_battle",
		"正在创建队伍战斗",
		self:_snapshot_of(player),
		request_id,
		deliveries
	)
	result.battle_request = {
		team_id = team.team_id,
		leader_id = team.leader_id,
		template_id = payload.template_id,
		player_ids = player_ids,
		args = payload.args,
	}
	return result
end

function Model:attach_battle(team_id, battle_entry)
	local team = self.teams[team_id]
	if not team or not team.membership_locked then
		return { ok = false, error = "team_not_found" }
	end
	team.battle_entry = battle_entry
	self:_bump_team(team)
	local deliveries = {}
	for _, player_id in ipairs(team.member_ids) do
		local player = self:_player(player_id)
		self:_deliver(
			deliveries,
			player_id,
			protocol.MESSAGE.BATTLE_READY,
			protocol.battle_ready_event(self:_snapshot_of(player), battle_entry)
		)
	end
	return { ok = true, deliveries = deliveries }
end

function Model:release_battle(team_id)
	local team = self.teams[team_id]
	if not team then
		return { ok = false, error = "team_not_found" }
	end
	if not team.membership_locked then
		return { ok = true, deliveries = {} }
	end
	team.membership_locked = false
	team.lock_reason = ""
	team.battle_entry = nil
	self:_bump_team(team)
	local deliveries = {}
	self:_push_team(team, deliveries)
	return { ok = true, deliveries = deliveries }
end

function Model:rollback_battle_start(team_id, request_id, message)
	local team = self.teams[team_id]
	if not team then
		return self:_error("team_not_found", "队伍不存在", request_id, {})
	end
	local released = self:release_battle(team_id)
	local leader = self:_player(team.leader_id)
	return self:_error(
		"battle_create_failed",
		message or "战斗创建失败，队伍已解锁",
		request_id,
		released.deliveries or {},
		{ snapshot = self:_snapshot_of(leader) }
	)
end

function Model:cancel_battle(team_id)
	local result = self:release_battle(team_id)
	if result.ok then
		result.message = "战斗创建已取消，队伍已解锁"
	end
	return result
end

function Model:handle_request(player_id, message_id, request_id, payload)
	local player = self.players[player_id]
	if not player or not player.online then
		return self:_error("not_authenticated", "连接未认证", request_id, {})
	end
	if type(payload) ~= "table" then
		return self:_error("invalid_request", "请求包体必须是 JSON 对象", request_id, {})
	end

	local expected_type = protocol.REQUEST_TYPES[message_id]
	if not expected_type then
		return self:_error("invalid_request", "未知的队伍消息号", request_id, {})
	end
	if payload.schema_version ~= protocol.SCHEMA_VERSION then
		return self:_error("invalid_request", "不支持的协议版本", request_id, {})
	end
	if payload.type ~= expected_type then
		return self:_error("invalid_request", "消息类型不匹配", request_id, {})
	end

	if expected_type == "invite_team" then
		return self:_invite(player, payload, request_id)
	elseif expected_type == "respond_invite" then
		return self:_respond_invite(player, payload, request_id)
	elseif expected_type == "leave_team" then
		return self:_leave_team(player, payload, request_id)
	elseif expected_type == "kick_member" then
		return self:_kick_member(player, payload, request_id)
	elseif expected_type == "sync_team" then
		return self:sync(player_id, request_id)
	elseif expected_type == "start_battle" then
		return self:_start_battle(player, payload, request_id)
	end
	return self:_error("invalid_request", "未处理的队伍消息", request_id, {})
end

function Model:status()
	local online_players = 0
	local player_count = 0
	for _, player in pairs(self.players) do
		player_count = player_count + 1
		if player.online then
			online_players = online_players + 1
		end
	end
	local team_count = 0
	for _ in pairs(self.teams) do
		team_count = team_count + 1
	end
	local invitation_count = 0
	for _ in pairs(self.invitations) do
		invitation_count = invitation_count + 1
	end
	return {
		players = player_count,
		online_players = online_players,
		teams = team_count,
		invitations = invitation_count,
		next_team_id = self.next_team_id,
	}
end

return Model
