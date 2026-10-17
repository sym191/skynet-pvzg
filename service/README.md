# 原版 Skynet 组队服务

基于 `pvz-global/team/功能文档.md` 和 `team_protocol.md`（2026-10-13 Team v3）实现。
业务代码、配置及测试集中于根目录 `service/`；根目录 `start-server.sh` 为启动入口。
只运行 `third_party/skynet` 原版服务，不依赖 C++ 重构版、CMake 或外部数据库。

## 启动

```bash
./start-server.sh
```

启动原版 Skynet 并注册内部服务 `.team`，前台运行，Ctrl+C 停止，可从任意目录执行。
首次运行缺少产物时直接用原版 Makefile 构建；子模块缺失时自动初始化。
需要 Linux、Bash、make、C 编译器及 util-linux 的 `flock`。
仅构建：`./start-server.sh --build-only`；帮助：`./start-server.sh --help`。

启动器不监听客户端端口，也不生成账户或提供登录协议。客户端接入、认证和 TCP 包头
由后续统一网络层负责；组队提供服务器内部接口。

| 环境变量 | 默认值 / 用途 |
| --- | --- |
| `TEAM_INVITE_TTL` | `60`，正整数秒 |
| `TEAM_DATA_DIR` | `<项目>/var/team` |
| `TEAM_STATE_PATH` | `<数据目录>/state.json` |
| `TEAM_BUILD_ROOT` | `<项目>/build/original` |
| `TEAM_BUILD_JOBS` | `2`，首次构建并行数 |
| `TEAM_BATTLE_SERVICE` | 空，不启动战斗适配器 |
| `TEAM_POLICY_MODULE` | `team.policy`，可替换邀请权限、队长继任策略 |

路径覆盖值使用绝对路径。启动器对状态文件加独占进程锁，避免多个实例同时写入。
数据与锁文件保存在 Git 忽略目录中。有效期配置示例：

```bash
TEAM_INVITE_TTL=120 ./start-server.sh
```

## 目录与服务边界

```text
service/
  main.lua                 启动、注册 .team、失败终止
  config.lua               原版 Skynet 配置
  common/json.lua          业务 JSON 编解码
  team/model.lua           队伍、邀请、状态版本、锁定与战斗入口
  team/policy.lua          生命周期默认策略
  team/service.lua         内部调用、玩家服务绑定、事务提交、业务通知
  team/store.lua           状态加载、同目录临时文件替换
  tests/                   模型测试与原版 Skynet 服务间集成测试
```

调用关系：统一网关 → 已认证的玩家会话服务 → `.team`。
组队由单个 Skynet 服务拥有全部状态，模型调用不进行 RPC、不 yield。
每次修改先写状态文件，再按提交顺序发出事件和响应。

## 玩家会话服务接入

每个玩家会话对应一个 Skynet 服务地址。登录成功后，由该服务使用服务端认证得到的资料
注册上线；不能把 `online` 暴露为客户端可调用命令，也不能从 Team 包体取得当前玩家身份。
组队信任服务器内部服务，账号认证的正确性由调用方负责。

```lua
-- 在已经完成认证的玩家会话服务内调用。
assert(skynet.call(".team", "lua", "online", {
    player_id = authenticated_player_id,
    display_name = player_display_name,
    avatar_key = player_avatar_key,
}))

-- 网络层解析包头后，仅转交 Team 消息号和业务 JSON。
local message_id, payload_json = skynet.call(".team", "lua", "request", 7,
    '{"schema_version":3,"type":"sync_team"}')
-- 响应包头、request_id 关联、flags 和实际发包由玩家会话/公共网络层处理。

-- 连接断开时、销毁玩家服务前必须报告下线。
skynet.call(".team", "lua", "offline")
-- 无需等待确认时可使用 skynet.send(".team", "lua", "offline")。
```

| 内部接口 | 结果 / 职责 |
| --- | --- |
| `online(profile)` | 返回 boolean；将调用服务地址绑定到认证玩家，更新在线状态 |
| `offline()` | 返回 boolean；解除调用地址绑定、更新离线状态，保留队伍归属 |
| `request(message_id, payload_json)` | 返回 `response_message_id, response_json`；按调用服务地址取得操作者 |
| `team_event(message_id, payload_json)` | `.team` 通过 `skynet.send` 发给玩家服务的业务通知 |
| `team_replaced()` | 新玩家服务接管同一玩家时发给旧服务；旧服务应停止客户端转发 |

玩家服务需在自己的 Lua dispatcher 中处理 `team_event` 和 `team_replaced`，并校验通知来源为
实际 `.team` 地址。业务响应和通知保留 JSON 字符串，避免跨服务序列化丢失 `[]`/`null`。
`request` 使用 `skynet.call` 获取结果；请求 ID、连接 ID 和 TCP 包头不进入组队接口。

同一玩家的新服务接管后，旧服务的请求返回 `not_authenticated`，迟到的 `offline` 不会影响
新服务在线状态。更换账号前必须先下线。玩家服务退出/异常时的清理由会话生命周期管理方
保证；组队不会自行通过网络连接推断上线、下线。

Team v3 六类业务消息及字段继续以共享文档为准：

| 请求 | 响应 | 行为 |
| --- | --- | --- |
| 3 `leave_team` | 103 | 离队者空快照、剩余成员同步、队长继任 |
| 4 `invite_team` | 104 | 自动建队、同队同目标有效邀请去重、110 通知 |
| 5 `respond_invite` | 105 | 接受再校验、拒绝也是成功、111 结果 |
| 6 `kick_member` | 106 | 仅队长、禁止踢自己、112 移出通知 |
| 7 `sync_team` | 101 | 当前完整快照、有效 invitations 数组、entry 或 null |
| 8 `start_battle` | 108 | 全员校验、原子锁定、失效邀请、异步调用战斗服务 |

未上线请求返回 255 / `not_authenticated`，无效 JSON/参数返回 255 / `invalid_request`。
每次状态变更也通知请求者，响应丢失不影响状态恢复。
快照版本按接收玩家跨队伍递增；同版本快照内容一致，空数组输出 `[]`。
邀请定时过期并通知双方；定时清理后的旧邀请返回 `invite_invalid`，在接受时发现到期则返回
`invite_expired`。被踢离线玩家的空快照和原因持久化，下次上线发送 112。

## 生命周期与存储

功能文档 2.1 尚标为建议。本实现暂采用以下服务端默认值，未将共享文档改为已确认需求：

- 所有队员可以邀请；踢人、开战仅队长。
- 队长主动离队按加入顺序优先选择在线成员，否则选择最早剩余成员。
- 队长掉线保留身份；离线成员占名额；全员离线保留队伍。
- 最后一人主动离队销毁队伍；邀请拒绝/过期后保留单人队伍。
- 战斗结束保留队伍，只有可信战斗服务确认后解除锁定。

队伍最多 5 人。邀请不占名额；最后一个名额竞争、接受邀请与开战按服务端提交顺序决定。
成员在线、活动限制、锁定和成员关系变化均推进受影响玩家版本。

状态文件保留成员关系、玩家版本、邀请、离线移出原因、战斗锁定及已分配入口。
重启将旧会话全部标记离线并推进版本，随后由玩家服务重新报告上线，不重置队伍或版本。
存储采用同步写临时文件再 rename，写入失败回滚该次模型操作；不会先发送成功结果。
断线状态无法落盘时停止服务，避免把断开的成员继续当作在线。
这是单进程、小规模部署的文件存储；没有数据库事务、多节点共享或断电 fsync 保证。
替换存储时仍须保持“持久化成功后发布”的顺序。

## 战斗服务接入

默认未配置战斗服务，开战返回 `battle_create_failed`，队伍保持未锁定。
组队模块不创建本地单机战斗、不生成假进入地址。配置真实适配器：

```bash
TEAM_BATTLE_SERVICE=battle/service ./start-server.sh
```

`battle/service` 应实现以下 Skynet Lua 命令（代码同样放在 `service/battle/`）：

| 组队发给适配器的命令 | 参数与职责 |
| --- | --- |
| `create_team_battle` | `team_service, job`；job 包含 `token, team_id, player_ids, template_id, params_json` |
| `recover_team_battle` | `team_service, team_id, token, entry`；重启核对持久战斗状态 |

`player_ids` 使用稳定加入顺序。`params_json` 是 JSON 字符串，避免 Skynet 序列化丢失
空数组/null 语义。适配器必须再次验证具体玩法条件，并用 `token` 实现持久幂等分配。
不要把调用超时、服务不可达解释为战斗已取消；此时队伍保持锁定，等待权威恢复确认。

适配器调用 `.team`（或传入的地址）：

```lua
-- entry 包含 battle_id、team_id、side_id、player_ids，可附地址/凭证。
-- 推荐传 JSON 字符串以保留扩展字段中的数组/null；基础字段也支持 Lua 表。
skynet.call(team_service, "lua", "battle_ready", team_id, token, entry_json)
-- 只有确认创建取消或战斗结束后才解锁；token 必须匹配当前锁。
skynet.call(team_service, "lua", "battle_finished", team_id, token)
-- 单人战斗等外部活动也需要设置，空字符串表示解除限制。
skynet.call(team_service, "lua", "set_activity", player_id, "正在参加其他战斗")
```

这些命令只允许已配置的战斗服务地址调用。`battle_ready` 验证名单、队伍和锁定 token，
向全员发送相同 battle_id/side_id 的 109；之后查询返回相同入口。旧战斗回调不能解除新锁。
锁定中在线状态继续更新，离线不会释放名额。房间、匹配、加载策略由后续模块实现。

## 验证

在项目根目录运行：

```bash
./start-server.sh --build-only
third_party/skynet/3rd/lua/lua service/tests/model_test.lua
python3 service/tests/integration_test.py
```

模型测试覆盖唯一归属、去重、容量、权限、失效/过期邀请、活动限制、锁定、队长继任、
离线恢复、跨队版本和业务 JSON 格式。
集成测试在真实原版 Skynet 中创建模拟玩家服务，直接通过 `skynet.call/send` 验证业务请求、
通知、身份绑定、并发争抢名额、离线踢人、邀请过期、战斗取消/结束、会话替换、进程重启及
存储写入失败回滚。另验证默认启动没有监听套接字、不生成账户文件，并保留状态文件互斥。
测试无需客户端、端口或登录凭据。`tests/battle_provider.lua` 仅用于验证战斗接入流程，
默认启动不加载它。
