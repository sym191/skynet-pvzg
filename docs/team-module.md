# 原版 skynet Team 模块

## 1. 范围

本模块运行在 `third_party/skynet` 提供的原版 skynet 上，不依赖 `src/` 中的 C++23 重构运行时。
业务规则和消息语义参考 `pvz-global/team/功能文档.md` 与 `pvz-global/team/team_protocol.md`，
当前实现采用 Team v3、JSON payload、40 字节 PVZN 包头和 schema_version=3。

已实现：

- 未组队玩家邀请好友时自动建队，创建者自动成为队长。
- 邀请、接受、拒绝、过期、去重、失效和重连恢复。
- 离队、队长继任、踢人、离线成员占位和离线被踢后的恢复通知。
- 每队最多 5 人，每名玩家最多属于一个队伍。
- 成员在线状态、战斗准入状态、战斗锁定及锁定期间成员关系变更拦截。
- 每名接收玩家独立的 `state_version`，用于下行快照防回退。
- `sync_team` 返回队伍快照、有效邀请和已分配战斗入口。
- 队长开战、统一战斗入口、解锁事件。
- 40 字节包头编解码、JSON 编解码和 TCP 粘包流解析。

未包含：

- 登录、鉴权和会话服务。Team 服务只接受网关传入的已认证 `player_id`。
- 正式客户端网关和连接管理。仓库提供协议编解码与服务接口，由网关负责收发包。
- 跨进程持久化和多节点分片。当前状态保存在单个 skynet `team` 服务内。
- 正式战斗服务。`team_battle` 是可替换的参考实现。

## 2. 文件布局

| 路径 | 职责 |
| --- | --- |
| `lua/lualib/team/json.lua` | 无第三方依赖的 JSON 编解码 |
| `lua/lualib/team/frame.lua` | PVZN 40 字节包头、粘包解析和 payload 编解码 |
| `lua/lualib/team/protocol.lua` | Team v3 消息号、flags、快照和事件构造 |
| `lua/lualib/team/model.lua` | 队伍、邀请、版本、锁定和战斗状态机 |
| `lua/service/team.lua` | skynet `team` 服务及内部 Lua 接口 |
| `lua/service/team_battle.lua` | 可替换的战斗入口参考服务 |
| `lua/service/main.lua` | 原版 skynet 节点启动入口 |

## 3. 启动与配置

构建并启动原版 skynet：

```bash
tools/build.sh original debug
tools/run-original.sh
```

`main.lua` 会启动 `team_battle` 和 `team` 两个 uniqueservice。

`configs/skynet_original.lua.in` 中的配置项：

| 配置 | 默认值 | 说明 |
| --- | --- | --- |
| `team_invite_ttl_seconds` | `"60"` | 邀请有效期，单位秒 |
| `team_invite_permission` | `"all"` | `all` 表示所有成员可邀请，`leader` 表示仅队长 |
| `team_leader_succession` | `"online_first"` | 队长主动离队时优先选择最早加入的在线成员 |
| `team_battle_service` | `"team_battle"` | 战斗入口服务名，空字符串表示未接入 |

`team_invite_permission`、队长继任和邀请有效期属于功能文档 2.1 中尚未最终确认的策略，
因此通过配置集中表达，不散落在业务代码中。

## 4. 网关对接

### 4.1 身份边界

Team 服务不读取 JSON 中的玩家 ID 作为身份。网关必须先完成鉴权，再把认证后的
`player_id`、`display_name`、`avatar_key` 传给 Team 服务。

客户端包体中的 `target_player_id` 只表示操作目标，不表示请求者身份。

### 4.2 建立连接

```lua
local result = skynet.call("team", "lua", "connect", {
    player_id = authenticated_player_id,
    display_name = display_name,
    avatar_key = avatar_key,
})
```

返回：

```lua
{
    player_id = "player_a",
    sync = { ... },          -- 完整 TeamSnapshot、invitations、entry
    deliveries = { ... },    -- 需要发给在线玩家的推送
}
```

网关应将 `sync` 作为登录/重连后的 101 状态同步发送给当前连接，再按 `deliveries`
投递离线期间产生的被踢通知和队伍状态变化。

断线时调用：

```lua
skynet.call("team", "lua", "disconnect", player_id)
```

断线只把成员标记为离线，不释放名额、不自动离队。

### 4.3 处理客户端请求

网关按 `frame.lua` 解析 TCP 数据，校验包头后调用：

```lua
local result = skynet.call(
    "team",
    "lua",
    "request",
    authenticated_player_id,
    packet.message_id,
    packet.request_id,
    decoded_json_payload
)
```

`result` 结构：

```lua
{
    request_id = 123,
    message_id = 104,
    flags = 2,
    payload = { ... },       -- Team v3 JSON 响应
    deliveries = {
        {
            player_id = "player_b",
            message_id = 110,
            flags = 4,
            payload = { ... },
        },
    },
}
```

网关先用 `team.frame.encode` 封装 `result.payload` 并回填原请求 `request_id`，
再把每个 `delivery` 编码后投递给对应在线连接。业务错误使用
`message_id=255`、`flags=RESPONSE|ERROR=10`。

### 4.4 TCP 流解析

`team.frame.try_decode(buffer)` 返回：

- `packet, rest, nil`：成功解析一帧，`rest` 继续处理。
- `nil, buffer, nil`：数据不足，保留原缓冲区等待更多字节。
- `nil, nil, error_message`：包头非法，网关应关闭连接。

包头格式为 `!IHHHHHHQIIII`，字段和消息号以 `pvz-global/team/team_protocol.md` 为准。
payload 最大 1 MiB，`team.frame.decode_json(packet)` 解析 JSON 包体。

## 5. Team 服务内部接口

| 命令 | 参数 | 用途 |
| --- | --- | --- |
| `connect` | principal 表 | 标记在线并返回完整同步状态 |
| `disconnect` | `player_id` | 标记离线并推送成员在线变化 |
| `request` | `player_id, message_id, request_id, payload` | 处理 Team v3 C2S 请求 |
| `sync` | `player_id` | 查询当前快照、邀请和战斗入口 |
| `set_battle_eligibility` | `player_id, can_enter, reason` | 由活动/玩法模块更新准入状态 |
| `release_battle` | `team_id` | 战斗结束后解除成员锁定 |
| `status` | 无 | 返回服务内玩家、队伍和邀请计数 |

所有状态变更由单个 `team` 服务串行提交，因此自动建队、最后名额竞争、接受邀请与开战锁定
都使用同一提交顺序，不会因为多个请求协程并发而超员或出现双重归属。

## 6. 战斗服务对接

`start_battle` 校验并通过后，Team 服务先锁定名单、使未完成邀请失效，再调用：

```lua
skynet.call(team_battle, "lua", "create_team_battle", {
    team_id = team_id,
    leader_id = leader_id,
    template_id = template_id,
    player_ids = { ... },  -- 稳定加入顺序
    args = request_args,
})
```

战斗服务至少返回：

```lua
{
    battle_id = "battle_000001",
    side_id = "side_team_000001",
}
```

Team 服务补全 `team_id` 和 `player_ids` 后向所有在线成员发送 109。
正式战斗服务还需负责实际战斗生命周期；结束时由可信服务调用 `release_battle` 解锁。
创建失败时 Team 服务会回滚本次锁定并返回 `battle_create_failed`。

## 7. 业务规则摘要

- 队伍上限 5 人，离线成员继续占名额。
- 玩家没有队伍时发起邀请，会在执行本次邀请前自动建队。
- 邀请发送成功与入队成功严格区分；只有接受并通过再次校验才修改成员关系。
- 同队对同一目标的有效邀请去重。
- 目标离线、已有队伍、已在本队、队伍满员、锁定或活动状态不允许时返回明确错误码。
- 战斗锁定后禁止邀请、接受邀请、离队和踢人；在线状态仍持续同步。
- 状态版本按接收玩家递增。队伍、成员、在线状态、准入状态和锁定变化都会推进版本。
- 被踢的离线成员重连时收到 112 和空快照。
- 最后一名成员离队后销毁队伍；队长主动离队时按配置继任。

## 8. 验证

纯 Lua 测试：

```bash
lua tests/team/unit_test.lua "$PWD"
```

原版 skynet 集成测试：

```bash
ctest --preset linux-debug -R '^team\.' --output-on-failure
```

集成测试启动真实 `skynet` 二进制，加载 `team` 和 `team_battle` 服务，覆盖连接、自动建队、
邀请、接受、同步、开战入口、解锁、断线和重连。

## 9. 后续接入

1. 网关实现登录鉴权，并把认证后的 principal 传给 `team.connect`。
2. 网关接入 `frame.try_decode`、`team.request` 和 delivery 投递。
3. 活动/玩法服务上报 `can_enter_battle` 和阻塞原因。
4. 战斗模块替换 `team_battle` 参考服务，并在战斗生命周期结束时调用 `release_battle`。
5. 如需跨进程恢复，再接入持久化存储；当前内存状态不跨 skynet 节点重启保留。
