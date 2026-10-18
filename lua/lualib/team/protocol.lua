local json = require "team.json"

local protocol = {}

protocol.SCHEMA_VERSION = 3
protocol.SERVICE_ID = 3

protocol.FLAGS = {
	REQUEST = 1,
	RESPONSE = 2,
	EVENT = 4,
	ERROR = 8,
	ACK = 16,
}

protocol.MESSAGE = {
	LEAVE_TEAM_REQUEST = 3,
	INVITE_TEAM_REQUEST = 4,
	RESPOND_INVITE_REQUEST = 5,
	KICK_MEMBER_REQUEST = 6,
	SYNC_TEAM_REQUEST = 7,
	START_BATTLE_REQUEST = 8,
	TEAM_SNAPSHOT = 101,
	LEAVE_TEAM_RESPONSE = 103,
	INVITE_TEAM_RESPONSE = 104,
	RESPOND_INVITE_RESPONSE = 105,
	KICK_MEMBER_RESPONSE = 106,
	START_BATTLE_RESPONSE = 108,
	BATTLE_READY = 109,
	INVITATION = 110,
	INVITE_RESOLVED = 111,
	REMOVED = 112,
	ERROR = 255,
}

protocol.REQUEST_TYPES = {
	[protocol.MESSAGE.LEAVE_TEAM_REQUEST] = "leave_team",
	[protocol.MESSAGE.INVITE_TEAM_REQUEST] = "invite_team",
	[protocol.MESSAGE.RESPOND_INVITE_REQUEST] = "respond_invite",
	[protocol.MESSAGE.KICK_MEMBER_REQUEST] = "kick_member",
	[protocol.MESSAGE.SYNC_TEAM_REQUEST] = "sync_team",
	[protocol.MESSAGE.START_BATTLE_REQUEST] = "start_battle",
}

protocol.RESPONSE_MESSAGES = {
	leave_team = protocol.MESSAGE.LEAVE_TEAM_RESPONSE,
	invite_team = protocol.MESSAGE.INVITE_TEAM_RESPONSE,
	respond_invite = protocol.MESSAGE.RESPOND_INVITE_RESPONSE,
	kick_member = protocol.MESSAGE.KICK_MEMBER_RESPONSE,
	sync_team = protocol.MESSAGE.TEAM_SNAPSHOT,
	start_battle = protocol.MESSAGE.START_BATTLE_RESPONSE,
}

protocol.array = json.array
protocol.null = json.null

function protocol.member(player)
	local can_enter_battle = player.online and player.can_enter_battle
	local reason = player.battle_block_reason or ""
	if not player.online then
		reason = "成员离线"
	elseif not player.can_enter_battle and reason == "" then
		reason = "成员当前不可进入战斗"
	end
	return {
		player_id = player.player_id,
		display_name = player.display_name,
		avatar_key = player.avatar_key or "",
		online = player.online,
		can_enter_battle = can_enter_battle,
		battle_block_reason = can_enter_battle and "" or reason,
	}
end

function protocol.empty_snapshot(state_version)
	return {
		state_version = state_version,
		team_id = "",
		leader_id = "",
		members = protocol.array(),
		membership_locked = false,
		lock_reason = "",
	}
end

function protocol.snapshot(state_version, team_id, leader_id, members, membership_locked, lock_reason)
	return {
		state_version = state_version,
		team_id = team_id,
		leader_id = leader_id,
		members = protocol.array(members),
		membership_locked = membership_locked,
		lock_reason = lock_reason or "",
	}
end

local function add_extras(payload, extras)
	if not extras then
		return payload
	end
	for key, value in pairs(extras) do
		payload[key] = value
	end
	return payload
end

function protocol.success(message_type, message, snapshot, extras)
	return add_extras({
		schema_version = protocol.SCHEMA_VERSION,
		type = message_type,
		ok = true,
		message = message,
		snapshot = snapshot,
	}, extras)
end

function protocol.error(code, message, extras)
	return add_extras({
		schema_version = protocol.SCHEMA_VERSION,
		type = "error",
		ok = false,
		error = code,
		message = message,
	}, extras)
end

function protocol.snapshot_event(snapshot)
	return {
		schema_version = protocol.SCHEMA_VERSION,
		type = "team_snapshot",
		snapshot = snapshot,
	}
end

function protocol.sync_response(snapshot, invitations, entry)
	local invitation_values = protocol.array()
	for _, invitation in ipairs(invitations) do
		invitation_values[#invitation_values + 1] = invitation
	end
	return {
		schema_version = protocol.SCHEMA_VERSION,
		type = "team_snapshot",
		snapshot = snapshot,
		invitations = invitation_values,
		entry = entry or json.null,
	}
end

function protocol.invitation_summary(invitation, inviter)
	return {
		invitation_id = invitation.invitation_id,
		team_id = invitation.team_id,
		inviter = {
			player_id = inviter.player_id,
			display_name = inviter.display_name,
			avatar_key = inviter.avatar_key or "",
		},
		expires_at = invitation.expires_at,
		expires_in_sec = invitation.expires_in_sec,
	}
end

function protocol.invitation_event(invitation, inviter)
	local event = protocol.invitation_summary(invitation, inviter)
	event.schema_version = protocol.SCHEMA_VERSION
	event.type = "invitation"
	return event
end

function protocol.invite_resolved_event(invitation_id, outcome, message, error_code)
	local event = {
		schema_version = protocol.SCHEMA_VERSION,
		type = "invite_resolved",
		invitation_id = invitation_id,
		outcome = outcome,
		message = message,
	}
	if error_code then
		event.error = error_code
	end
	return event
end

function protocol.removed_event(snapshot, message)
	return {
		schema_version = protocol.SCHEMA_VERSION,
		type = "removed",
		snapshot = snapshot,
		message = message,
	}
end

function protocol.battle_ready_event(snapshot, entry)
	return {
		schema_version = protocol.SCHEMA_VERSION,
		type = "battle_ready",
		snapshot = snapshot,
		entry = entry,
	}
end

return protocol
