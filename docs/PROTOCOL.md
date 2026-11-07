# PartyUp 协议（草案 v0.1）

> 状态：草案。消息形状已定，字段名与错误码可能调整；实现前以本文为准，改协议先改本文。

## 0. 通用信封

WebSocket 承载全部控制消息，一条连接对应一个成员；同一成员可以同时建立多条连接（在线状态按成员聚合，见 §7.2）。

```json
{ "v": 1, "id": "01J...", "type": "text.send", "ts": 1767225600000, "payload": {} }
```

- `v`：协议版本，当前恒为 1。服务端在 `hello` 里返回自己支持的范围。
- `id`：客户端生成的 ULID，用于请求与响应配对；服务端推送的消息也带 `id`。
- `type`：点分命名，`<域>.<动作>`；响应为 `type + ".ok"` 或 `type + ".err"`。
- `ts`：毫秒 epoch，客户端时间仅供参考，服务端以自己时间为准。

错误：

```json
{ "v": 1, "id": "01J...", "type": "text.send.err", "ts": 1767225600000,
  "payload": { "code": "FORBIDDEN", "message": "not a member of this channel" } }
```

错误码（第一批）：`UNAUTHENTICATED` `FORBIDDEN` `NOT_FOUND` `CONFLICT` `RATE_LIMITED` `INTERNAL` `INVALID_ARGUMENT`。

`INVALID_ARGUMENT` 是 2026-11-02 补入的：载荷形状非法、字段缺失/越界、消息类型未知都归它，HTTP 400 与 WS 参数错误共用同一个稳定名字。

`SESSION_REPLACED` 是 2026-11-07 补入的：token 本身格式合法，但已被同一账号**更新的一次登录**
撤销（单端登录，见 §1.2）。它与 `UNAUTHENTICATED` 分开，客户端据此提示「账号已在另一处登录」
而不是「登录已失效」。

## 1. 握手与认证

1. 建立 WS → 客户端 30 秒内发 `auth`。
2. `auth { token, capabilities? }` → `auth.ok { member, server }`，否则 `auth.err` 后关闭连接。
   可选 `capabilities` 是字符串数组，客户端用它 opt-in 新行为；不传 = v1 兼容。
   `auth.ok.server.capabilities` 是服务端广告能力：
   - `screen.watch.v1`：服务端支持 `screen.watch`/`screen.unwatch`；
   - `screen.watch.slots.v1`：`screen.watch.ok` 里的 `slot`/`generation` 有效（relay 槽位级帧归属）；
   - `screen.watch.isolated.v1`（B2）：服务端支持**每次 watch 新建独立 receive-only
     PeerConnection** 的严格接收传输。只在 `--screen-mode=relay` 且视频通道可用时广告。
   - 本地媒体核心还需 `pu_engine_caps` bit5 与 receive 导出；缺任一层都必须明确报不支持。
   旧服务端不会广告这些项；客户端必须检查后再生效，不能把「导出存在」当能力。
3. 服务端每 20 秒发 `ping`，客户端回 `pong`；60 秒无 pong 断开。

### 1.1 认证失败错误码（2026-11-07 起区分）

`auth.err` 的 `code`：

| code | 含义 | 客户端建议 |
| --- | --- | --- |
| `UNAUTHENTICATED` | token 缺失、无效，或**已过期**（`expires_at` 已过） | 清本地 token，回登录页 |
| `SESSION_REPLACED` | token 曾有效，但已被该账号**更新的一次登录**撤销（单端登录，§1.2） | 停止重连，提示「账号已在另一处登录」 |
| `MEMBER_BANNED` | 账号已被管理员封禁（§5.1）；登录、`/api/me`、`/api/members`、WS 鉴权一律拒绝 | 停止重连，提示「账号已被封禁」并展示申诉方式 |
| `OWNER_ONLY` | 该操作只有 owner 能做（任免管理员、危险设置、转让所有权，§2） | 隐藏入口 + 提示权限不足 |
| `ADMIN_ONLY` | 该操作至少需要 admin（普通成员访问管理端点） | 隐藏入口 + 提示权限不足 |
| `NOT_BOOTSTRAPPED` | 服务器还没有 owner（未初始化） | 走首次引导流程 |
| `ALREADY_BOOTSTRAPPED` | 服务器已初始化，bootstrap 被拒绝（§7.4） | 直接进登录页 |

`auth.err` 之后服务端立即关闭连接。对**已鉴权**连接，因会话被替换/注销而结束时，服务端先发一条
`session.replaced`（§1.2）应用层消息，再用 WebSocket 关闭码 **4001**（4000–4999 应用私有段）
关闭；客户端应以应用层消息的 `reason` 决定提示文案，关闭码只作诊断兜底。

HTTP 侧：

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| POST | `/api/bootstrap` | 首次启动、24 小时内用 stdout 打印的 bootstrap token 创建管理员 |
| POST | `/api/login` | 用户名 + 密码 → token |
| POST | `/api/invite/validate` | **非消费**预检邀请码（2026-11-06 补入；body 带 code，不把码放路径里） |
| POST | `/api/invite/{code}` | 用邀请码注册并直接登录（事务内重新校验 + 消费） |
| POST | `/api/register` | **开放注册**（`registration=open` 时可用；无需邀请码，新成员 role=user，2026-11-07 补） |
| GET | `/api/members` | 认证后取全服务器名册（含离线）；见 §7 |
| GET | `/api/me` | 校验 token、取当前成员信息 |
| POST | `/api/logout` | 退出登录：撤销 Bearer token 并关闭其 WS 连接；**204 无 body、幂等**（2026-11-07 补入） |
| GET | `/api/server-info` | **匿名**探测服务器状态（名称/版本/是否已初始化/注册策略/传输模式/bootstrap_allowed）；§7.3 |
| PATCH | `/api/me` | 改自己的展示名（所有角色） |
| POST | `/api/me/password` | 改自己的密码（需旧密码；所有角色） |
| GET/PATCH | `/api/admin/...` | 服务器管理（成员/频道/邀请/设置/审计/所有权）；需要 admin 或 owner，§9 |

Token 是不透明随机串（32 字节，base64url），存在 SQLite 里、可撤销——不用 JWT，因为需要"踢下线"能力。

HTTP 请求/响应体形状（2026-11-02 落地，2026-11-06 补 display_name / 预检 / 名册）：

| 路径 | 请求体 | 成功响应体 |
| --- | --- | --- |
| `POST /api/bootstrap` | `{ "token", "username", "password", "display_name"? }` | `{ "member": { id, name, display_name, admin }, "token" }` |
| `POST /api/login` | `{ "username", "password" }` | 同上（member 带库中 display_name） |
| `POST /api/invite/validate` | `{ "code" }` | `{ "valid": true }`；不存在 404、已用 409 |
| `POST /api/invite/{code}` | `{ "username", "password", "display_name"? }`（邀请码在路径里） | 同上 |
| `POST /api/register` | `{ "username", "password", "display_name"? }` | 同上（仅 `registration=open`；`invite`/`closed` → 403，到 `member_limit` → 409） |
| `GET /api/members` | — | `{ "members": [ { id, name, display_name, admin, online, state, voice_channel_ids } ] }`（§7） |
| `GET /api/me` | — | `{ "member": { id, name, display_name, admin } }` |
| `POST /api/logout` | — | `204 No Content`（无 body）；token 不在库里 → 401 |

**`display_name`（2026-11-06 补入）**：展示名，独立于唯一的登录名 `name`。注册/引导时可选；
未提供或空白回退到 `username`。约束为 1–32 个可打印 Unicode 字符（不含控制/换行/零宽字符），
**不要求唯一，也绝不能当身份 ID**；`name` 的唯一性与登录语义完全不变。旧库/旧行在启动时
加法迁移并回填为登录名，旧客户端请求不带该字段照常工作。所有成员 DTO 同时给 `name`（登录名）
与 `display_name`（展示名），旧字段语义不变。

**邀请码预检（2026-11-06 补入）**：`/api/invite/validate` 只回答「现在还能不能用」，
**不建成员、不消费邀请码、不返回创建者/使用者等成员信息**。预检成功不代表占用；
最终 `POST /api/invite/{code}` 仍在同一事务里重新校验并消费，所以预检后被别人用掉时注册会
返回 409。之所以用 POST + JSON body 而不是 `GET /api/invite/{code}`，是为了避免邀请码进入
访问日志/代理日志。

**开放注册（2026-11-07 补入）**：`registration=open` 时另有匿名 `POST /api/register`，
无需邀请码即可注册并直接登录。语义与邀请注册一致（同一套用户名/密码/显示名校验、
同一 `member_limit`、同一登录限速器按 IP 计数、成功清零），唯一区别是没有邀请码；
新成员固定 **role=user**（owner 只能由 bootstrap 产生，admin 只能由 owner 任免）。
`registration=invite` 时该端点返回 403「需要邀请码」，`closed` 时返回 403「注册已关闭」。
公开部署若选 `open`，应同时设置 `member_limit` 并保留登录限速。

**邀请链接（REG-03，2026-11-06 补入）**：正式流程仍是「服务器地址 + 邀请码」，邀请链接只是它们的
可复制形式：`https://example.org/partyup/?code=ABCDE12345`。它是 HTTP(S) base URL 加**恰好一个**
非空 `code` 查询参数，不是浏览器注册页，也不是 `partyup://` 深链；链接里的服务器只是连接目标，
**不代表已信任该服务器**。服务端不新增匿名发码 API：`partyup invite --server-url URL` 在原有
`INVITE_CODE` 输出后额外打印一行 `INVITE_LINK`；URL 由管理员显式声明，程序不读 Host/Forwarded、
不推断外部地址，非法 URL 在创建邀请码之前拒绝（不留下可用码）。

导入端只接受绝对 HTTP(S)、合法 host/port、无 user-info 或 fragment；query 必须恰好一个 `code`，
重复 code、空 key、未知参数、二义性一律拒绝，不默默取第一个/最后一个；`code` 归一化与注册路径
一致（去首尾空白、大写、去掉 `-`/空格/`_`）。导入是纯解析：不发 HTTP 请求、不打开浏览器、
不消耗邀请码，是否可用仍由 `/api/invite/validate` 与注册事务裁决。反代前缀必须保留：
客户端 HTTP 使用相对 `api/...`（BaseAddress 以 `/` 结尾），WebSocket 为 `<base>/ws`，
root 部署路径不变（`/api/...`、`/ws`）。

失败统一为 `{ "code": "<错误码>", "message": "<人类可读>" }`，状态码：400 参数非法（`INVALID_ARGUMENT`）、401 未认证（`UNAUTHENTICATED`）、403 禁止（`FORBIDDEN`）、404 不存在（`NOT_FOUND`）、409 冲突（`CONFLICT`）、429 限速（`RATE_LIMITED`，带 `Retry-After` 头）、500 内部（`INTERNAL`）。

### 1.2 会话生命周期：过期、滑动续期、单端登录（2026-11-07 补入）

**过期**：服务端 `sessions` 表有 `expires_at`（毫秒 epoch），默认 TTL **30 天**（启动参数
`--session-ttl`，Go duration，如 `720h`）。token 的判据是「未撤销且未过期」，`GET /api/me`、
`GET /api/members` 与 WS `auth` 都用它。旧库升级时按 `created_at + 默认 TTL` 回填，
不会一次性踢掉所有老 token。

**滑动续期**：每次 WS `auth` 成功与每次 `GET /api/me` 命中，服务端把 `expires_at` 推到
`now + TTL`。写库有 **5 分钟粒度节流**：只有「剩余有效期 < TTL − 5m」时才写，活跃连接最多
每 5 分钟一次单行 UPDATE，避免写放大。TTL ≤ 5 分钟时节流条件恒不成立、续期不触发
（TTL 到点即失效）——这是有意行为，便于用短 TTL 做验收。

**单端登录**：同一账号**同时只允许一端登录**。任何登录成功（`/api/login`、`/api/bootstrap`、
`/api/invite/{code}` 注册即登录）都在**同一事务**里先撤销该成员全部其他会话、再签发新 token，
所以任一时刻每个成员最多一条有效 session；并发登录也只有最后提交的那条有效。旧端会收到：

```json
{ "v": 1, "id": "01J...", "type": "session.replaced", "ts": 1767225600000,
  "payload": { "reason": "login_elsewhere" } }
```

随后连接被**与 reason 对应的关闭码**关闭。`reason` / 关闭码取值：

| reason | 关闭码 | 场景 | 客户端提示 |
| --- | --- | --- | --- |
| `login_elsewhere` | **4001** | 该账号在别处登录成功，本会话被替换 | 「账号已在另一处登录」 |
| `kicked` | **4002** | 被管理员踢下线（`POST /api/admin/members/{id}/kick`） | 「你已被管理员移出服务器」 |
| `banned` | **4003** | 被管理员封禁（`POST /api/admin/members/{id}/ban`） | 「账号已被封禁」；之后再登录得到 403 `MEMBER_BANNED` |
| `logout` | 4001 | 该 token 在别的连接上调用 `POST /api/logout` 主动退出 | 按主动退出处理（客户端已在退出流程里，不提示） |

`login_elsewhere` 的 4001 语义自 batch 1 起不变；4002/4003 是 2026-11-07 补的区分码，
**旧客户端按关闭码兜底仍能工作**（它们只会把后两者当普通掉线）。服务端对未知 reason 回退 4001。

服务端在 WS `auth` 时再次校验撤销/过期：被替换的 token 返回 `auth.err` + `SESSION_REPLACED`，
过期/未知 token 返回 `UNAUTHENTICATED`。

**退出登录**：`POST /api/logout` 带 `Authorization: Bearer <token>` → `204 No Content`（无 body），
同时关闭该 token 的所有活动 WS 连接（先发 `session.replaced`，`reason=logout`）。幂等：同一 token
重复调用仍 204；token 不在库里 → 401 `UNAUTHENTICATED`。运维/验收可用
`partyup sessions --data DIR [--active]` 直接核对会话表（只打印 token 哈希前缀，不打印原文）。

### 1.3 登录失败限速（2026-11-07 补入）

按 **IP + 用户名** 维度做进程内 token bucket，默认 **10 次失败 / 15 分钟**（启动参数
`--login-rate-limit 10/15m`，传 `0` 关闭）。超过上限返回 `429 Too Many Requests`，带
`Retry-After: <秒>` 响应头，body 仍是统一错误形状（`code=RATE_LIMITED`）。**成功登录清零**
该键；限速只作用于登录尝试，已签发的 token 与在线 WS 完全不受影响（别人试密码不会把在线
用户踢下线）。判定在任何密码校验（argon2）之前执行，避免被并发尝试打满 CPU。

### 1.4 服务器配置下发（`server.info`）

客户端鉴权后请求一次，缓存起来：

- `server.info {}` → `server.info.ok { stun_urls: ["stun:192.168.1.10:3478"] }`

`stun_urls` 是服务端内建 STUN responder（`--stun-listen` / `--stun-advertise`）的地址，
客户端必须拿它作为 PeerConnection 的 ICE servers 做候选收集——**不得硬编码公共 STUN**：
自托管场景下只有服务端知道自己对外可达的地址，公共 STUN 还会把客户端 IP 泄露给第三方。
空数组表示运维没开 STUN：仍可直连，但拿不到 srflx 候选，跨 NAT 通常连不上。

**为什么单独一条消息、而不是塞进 `voice.state`**：

1. **时序**：`stun_urls` 必须在**创建任何 PeerConnection 之前**拿到（要写进 ICE 配置），
   而 `voice.state` 是加入语音房间之后才广播的。塞进 voice.state 会造成「第一次进语音
   拿不到 STUN、第二次才有」的间歇性 bug。
2. **归属**：STUN 是服务器级属性，屏幕直连也要用，不该耦合到语音房间状态里。
3. **可扩展**：以后版本下限、频道上限、TURN 等都放这里，不再开新消息。

`server.info` 只回**配置**，不含任何传输策略选择信息——客户端不能据此推断自己可以选
relay/direct（那是 `voice.state.transport` 与服务端策略的事）。

### 1.5 角色与权限（2026-11-07 补入）

服务端只有三种角色，`members.role` 是**唯一真源**：

| role | 说明 |
| --- | --- |
| `owner` | 全服唯一（数据库部分唯一索引保证）。可任免管理员、改危险设置、转让所有权；**不可被降级/踢出/封禁** |
| `admin` | 管理成员（踢/封/解封/重置密码）、频道、邀请码；**不能对 owner 做任何写操作**，"任免管理员"与"危险设置"也仅 owner |
| `user` | 只能改自己的显示名/密码 |

所有成员 DTO（`member` 对象、名册行、`/api/members`）同时给两个字段：

- `role`: `"owner" | "admin" | "user"` —— 新代码只认它；
- `admin`: bool 兼容位，恒等于 `role ∈ {owner, admin}`（旧客户端仍可用）。

权限**全部在服务端强制执行**，客户端隐藏按钮只是体验：越权一律 `403` + `OWNER_ONLY` / `ADMIN_ONLY`
（§1.1 码表）。

### 1.6 设置推送（`settings.state`，2026-11-07 补入）

**服务端 → 客户端**单向推送，无 `.ok/.err`：

- `settings.state { display_name, announcement, welcome_message, registration,
  invite_creators, invite_max_uses_per_code, member_limit, history_retention_days,
  global_mute, voice_mode, screen_mode, max_screen_shares }`

owner/admin 通过 `PATCH /api/admin/settings` 改成功后，服务端把**整份快照**广播给所有已鉴权连接；
客户端直接整体覆盖本地缓存（不做增量）。危险项（`voice_mode` / `screen_mode` /
`max_screen_shares`）仅 owner 可改，`admin` 尝试会得到 `OWNER_ONLY`。

`voice_mode` / `screen_mode` 热更只影响**之后**新建的语音房间/屏幕共享；已建立的
PeerConnection 不会因改配置被重建（避免把在线用户直接踢掉），重进语音/重开共享即走新路径。

## 2. 频道

- `channel.list` → `channel.list.ok { channels: [{ id, name, voice: [...], shares: [...] }] }`
- `channel.create { name }` → `channel.create.ok { channel: { id, name, voice: [], shares: [] } }`
  —— 2026-11-02 补入（原文档只有 list/subscribe/unsubscribe，没有创建入口）。任何已认证成员都可创建；
  `name` 为 1–32 个可见字符，**全局唯一**，撞名回 `CONFLICT`。创建后仍需 `channel.subscribe` 才会收到推送。
  `voice` / `shares` 在 v1 恒为 `[]`（空数组，**不是 `null`**）；共享状态以 `screen.state` 推送为准。
- `channel.subscribe { channel_id }` → 之后该频道的 `text.new` / `presence` / `screen.state` / `media.signal` 推给本连接。
  订阅成功后服务端立即补两份**初始快照**：`voice.state`（本频道语音成员与模式）与
  `screen.state`（当前已有共享；没有共享时不发空快照，客户端按空列表处理，模式见
  `voice.state.screen_mode`）。晚加入者因此不需要等别人再操作就能看到已存在的语音/共享。
- `channel.unsubscribe { channel_id }`

订阅是显式的：不订阅就不推送，避免大服务器把无关频道的消息刷给所有人。
同一成员可以有多条 WebSocket；订阅、语音与共享的清理按**连接**计权，单连接断开/退订
只释放该连接持有的状态，不会误清另一条连接的语音或共享。

## 3. 文字

- `text.send { channel_id, client_msg_id, body }` → `text.send.ok { id, seq, ts, client_msg_id }`
- 广播 `text.new { channel_id, id, seq, member_id, member_name, member_display_name, body, ts }`
- `text.history { channel_id, before_seq, limit }` → `text.history.ok { messages: [...] }`（元素与 `text.new` 同形）

`seq` 是**每频道单调递增的整数**，由服务端分配；客户端按 `seq` 排序，不依赖时间戳。`client_msg_id` 用于本地乐观显示与去重（服务端在 `.ok` 里原样带回）。

`member_name` 是 2026-11-02 补入的：原来只有 `member_id`，历史消息里的离线成员无法显示名字（客户端只能退化到 id 前缀）。服务端在 `text.new` 与 `text.history` 里都填；`member_id` 保留，用于身份关联。

`member_display_name` 是 2026-11-06 补入的展示名（见 §1）。`member_name` 仍是**唯一登录名**，
两个字段都下发：旧客户端继续读 `member_name`，新客户端优先显示 `member_display_name`；
`member_id` 仍是唯一身份关联，不要用显示名当 ID。

## 4. 语音

**传输方式由服务端策略决定，客户端无权也无需选择。** 语音与屏幕共享是**两套独立策略**
（ADR-0002「三类媒体各自决定路径」）：

- `--voice-mode=relay|direct`（`PARTYUP_VOICE_MODE`，默认 `relay`）只管**语音**；
- `--screen-mode=relay|direct`（`PARTYUP_SCREEN_MODE`，默认 `relay`）只管**屏幕共享**。
  `direct` 只能 1v1 观看（P2P mesh 未实现），第三个人看不到；多人共享必须用 `relay`。

两者可以不同：语音中继 + 屏幕直连是合法配置，反之亦然。客户端从
`voice.join.ok.transport`（语音）与 `voice.state.voice_mode` / `voice.state.screen_mode`
（两者）读取结果，再按结果走对应媒体路径。

> `--screen-mode` 自 2026-11-04 起默认 `relay`：SFU 视频通道已实现并验收（1 共享 + N 观看）。
> 默认值必须让"多数人能看到"——direct 只支持 1v1，不能当默认。

- `voice.join { channel_id }` —— 上行载荷**只有** `channel_id`。
  客户端即使多传一个 `mode` 字段也会被服务端忽略（协议不再定义该字段）。
  响应 `voice.join.ok { channel_id, transport, role }`，`transport ∈ { relay, direct }`
  是服务端实际策略值；`role ∈ { offerer, answerer }` 是**服务端给自己分配的直连协商角色**
  （见下条「角色由服务端裁决」），relay 下恒为 `answerer`。
- `voice.leave { channel_id }`
- `voice.set_mute { channel_id, muted }` / `voice.set_deafen { channel_id, deafened }`
  —— 上报本端静音/闭音状态。本地 ADM 状态仍由客户端自己控制；上报是为了让别人看见（`voice.state`）。
  客户端只有在本地 ADM 真正接受并读回确认新状态后才应更新 UI 并上报；`pu_voice_set_mute` /
  `pu_voice_set_deafen` 返回错误时必须保持本地 UI 的旧状态，不能把未确认的静音显示成已静音。
- 广播 `voice.state { channel_id, voice_mode, screen_mode,
  members: [{ member_id, muted, deafened, role, joined_seq?,
              transport, transport_reason }],
  slots?: [{ index, member_id, muted }] }`
  —— `voice_mode` / `screen_mode` 是两条独立策略的服务端实际值（`relay|direct`）；
  旧字段 `mode` **已删除**（v1 客户端同版本发布，无兼容负担）。

  **`transport` 按成员下发，不按频道下发（2026-11-04 硬要求）**：
  `members[].transport ∈ { relay, direct }` 表示**这个成员实际在用什么**（事实），
  `transport_reason ∈ { server_policy, direct_failed_fallback }` 说明为什么。
  v1 服务端给所有人填同一个值（行为上仍是频道级），但**协议结构必须是 per-member**——
  逐成员混合路径（含成员中继）是 v1 之后的方向，届时只改服务端填充逻辑，
  协议与客户端都不用重设计；若现在写成频道级单字段，将来拆分就是一次破坏性变更。

  注意两个字段语义不同、都要保留：
  - `voice_mode` = 服务端**策略**（配置成什么，来自 `--voice-mode`）；
  - `members[].transport` = 该成员**实际**使用的路径（将来可能因回落/混合而不同）。
  客户端应读自己的 `members[].transport` 决定媒体路径；缺该字段的老服务端才回退读 `voice_mode`。
  `joined_seq` 是同一频道内单调递增的加入序号（仅诊断/排序用，不跨会话）。
  `members` 按 `member_id` 排序，稳定可比对。

  **角色由服务端裁决（2026-11-03 定）**：`members[].role` 是每个成员在直连协商里的角色，
  客户端**不得**再按「谁先来/谁 id 小」自行推断——两端各自推断会在同时加入时都得出
  `answerer`（各自只看到对方、没看到自己），结果是**没人发 offer、完全没声音**。
  规则（服务端实现，`--voice-mode=direct` 时）：
  - 频道里只有自己 → `answerer`（先等着，不对着空气发 offer）；
  - 第二个人加入 → 新来者 `offerer`，先来的保持 `answerer`（两人里恰好一个 offerer）；
  - 第三人及以后 → `answerer`，且**不改动已在通话的两人的角色**
    （P2P mesh 暂不支持，但绝不能因为第三个人进来就把两人的 PC 拆了）。
  relay 模式下全员 `answerer`（服务端 SFU 才是 offerer）。

  客户端拿到 `role` 后若发现与本地 PC 的角色不一致（典型：两端同时加入，服务端把
  offerer 判给了对端），应 `pu_voice_leave` + `pu_voice_join(passive=正确值)` 重建一次
  本地语音会话即可纠正；不需要新增 ABI。
  `slots` 仅 relay 模式出现：**槽位序号 → 发布者**。它解决两件事：
  （a）客户端能把 native 侧 id 为 `partyup-slot-<index>` 的远端音轨映射到具体成员
  （后绑定的槽不再依赖随机的 msid）；
  （b）「谁在说话」与按人排查质量时能显示名字。槽位序号在房间里统一分配
  （发布者首次发声时取最小空位），所以所有人看到的映射是同一份；离开时释放、可被后来者复用。
  客户端订阅频道后服务端立即推一次快照，所以「本频道是直连还是中继」在加入语音之前就能显示。
  这取代了 2026-11-03 客户端临时代码：当时服务端还没有语音状态，客户端只能借
  `media.signal`（`media=voice, kind=candidate`）捎带 `{"type":"partyup.voice.presence",...}`。
  客户端接上本节的 `voice.state` 后应删除那条 stopgap。

**已从 v1 草案删除的两项**（2026-11-03 产品裁定）：

- `voice.set_mode` —— 普通成员无权修改传输方式。将来若要做，应是独立的管理面入口（带权限），
  不是成员协议的一部分。
- `CONFLICT`「频道模式不一致」——既然模式来自服务端配置，就不存在每频道可变模式，
  也不存在「先加入者定模式」这件事。

### 4.1 中继（服务端 SFU，服务端策略 `--voice-mode=relay`）

**服务端是 offerer**：`voice.join` 成功后（仅当服务端策略为 relay），服务端为该成员单独建一个
`PeerConnection`，并下发：

槽位说明：12 条 sendonly m-line 在 offer 时就各挂一条**占位轨**，其 track id 固定为
`partyup-slot-00`…`partyup-slot-11`。因此客户端看到的每条远端音轨 id 都是稳定的槽位号
（不会出现「后绑定的槽没有 msid、id 随机」），配合 `voice.state.slots` 就能把
「第几条远端音轨」翻译成「哪个成员」。真实发布者通过 `ReplaceTrack` 挂上，不改变已协商的 msid。

```json
{ "type": "media.signal", "payload": {
  "channel_id": "...", "target": "sfu", "from": "sfu", "from_name": "sfu",
  "media": "voice", "kind": "offer",
  "payload": "{\"type\":\"offer\",\"sdp\":\"v=0...\",\"session_id\":\"relay/<member_id>\"}" } }
```

客户端把 `payload`（不透明字符串）交给媒体核心 `setRemoteDescription`，再回：

```json
{ "type": "media.signal", "payload": {
  "channel_id": "...", "target": "sfu", "media": "voice", "kind": "answer",
  "payload": "{\"type\":\"answer\",\"sdp\":\"v=0...\"}" } }
```

服务端 `SetRemoteDescription(answer)` 之后媒体通路建立。ICE 候选随 SDP 一起下发（非 trickle）；
`kind=candidate` 也接受，留作将来 trickle 兼容。

**相对本文件 2026-11-03 之前的草案改了两处**（原草案是 `media.offer { transport, session_id, sdp }`
+ 客户端回 `media.answer { session_id, sdp }`）：

1. 合并成一条 `media.signal`，用显式 `target: "sfu"` 区分「对象是服务端」还是「对象是频道成员」。
   两条路径的 payload 长得一模一样（都是媒体核心的信封字符串），只靠 `media`/`kind` 猜一定会错；
   显式 `target` 让服务端与客户端都能一眼分流。`to`（成员 id）在 `target="sfu"` 时被忽略。
2. 明确「服务端是 offerer」。SFU 需要在成员进出时改变「收谁的音」；如果客户端是 offerer，
   每次成员变化都要服务端反向发起重协商，而 ABI v2 的媒体核心在 answerer 侧遇到第二个 offer
   会丢弃旧会话重建（见提交 `00501b1`），代价是重新 ICE。改成服务端 offerer 之后，配合
   **预协商槽位**，一次 offer/answer 就够：

- 每个成员的 SDP 固定包含 1 条 `recvonly` 音频 m-line（服务端收他的麦克风）+
  `MaxRemoteSlots = 12` 条 `sendonly` 音频 m-line（服务端往这些槽里填别人的声音）。
- 成员进出只在空槽上 `ReplaceTrack`，**不产生重协商**；客户端一辈子只处理一次 offer/answer。
- 服务端 `OnTrack` 收到某成员音频后，写进一条 `TrackLocalStaticRTP`，再挂到其他每个成员的
  空闲槽上 —— 这就是「A 的声音同时转给 B 和 C」。
- **槽位数 = 中继语音成员上限 = 12 人**（`MaxRemoteSlots`）。
  为什么不是 13：槽位是**「发布者 → 槽位」的全局唯一映射**（同一个发布者在所有接收端用同一个
  序号，`voice.state.slots` 才能只广播一份）。N 个成员会同时成为 N 个发布者，各需要 1 个槽位，
  所以 13 个成员要 13 个槽位 —— 12 个槽位只能容纳 12 个成员。
- **超过 12 人时 `voice.join` 直接回 `CONFLICT: channel voice is full (max 12)`**，
  不会让第 13 个人「加入成功但没人听得到」。这是 2026-11-04 容量测试抓到的静默降级，
  门禁在 `server/internal/server/voice.go`，护栏测试 `TestVoiceChannelCapacityGate`。
- 槽位数与产品上限（`PRODUCT.md`「单频道同时在线 2–12 人」）是**同一个数**：
  将来改产品上限，必须同步改 `sfu.MaxRemoteSlots`，否则会重现「上限 15 但第 13 个人是哑的」。
- 语音满**不影响文字**：同一个频道可以有任意多文字成员，只有语音进不去。
- 中继模式下**客户端不要给服务端发 offer**；收到 `kind=offer` 应理解成「服务端在协商」，只回 answer。

服务端实现见 `server/internal/sfu/room.go`（房间）与 `server/internal/server/voice.go`（协议接线），
端到端测试见 `server/internal/server/voice_relay_test.go`（2 人 / 3 人中继 + voice.state +
「服务端策略覆盖客户端意见」）。

### 4.1.1 direct 策略下

服务端**不发 offer**，只做 `media.signal` 转发（§4.2）：由成员之间自行 offer/answer
（1:1 直连）。服务端仍维护 `voice.state`（谁在语音、谁静音），并拒绝 `voice.set_mode`。

**direct 语音硬上限 = 2 人**（`DirectMaxMembers`，P2P mesh 只实现了 1v1）。
第 3 个成员 `voice.join` 直接收 `CONFLICT`：

```
当前服务器配置为直连模式，语音最多 2 人；需要更多人请让管理员用 --voice-mode=relay 重启服务器
```

错误信息里带**出路**（改配置）是刻意的：不给用户出路，用户只会以为产品坏了。
与 relay 的槽位门禁同类——direct 下第 3 人如果被放进来，同样会「能进但互相听不到」，
属静默降级，所以必须拒绝。语音满**不影响文字**。

### 4.2 直连

服务端只做中转与定向投递，不解析也不改写 SDP：

- `media.signal { channel_id, media, kind, payload, target?, to? }`
  - **`media` 必填，`media ∈ { voice, screen }`，且是路由的唯一依据**：
    - `media=voice` + `voice_mode=relay` → 交给服务端 SFU（§4.1，服务端是 offerer）
    - `media=voice` + `voice_mode=direct` → 成员间转发（offer 广播 / answer·candidate 定向）
    - `media=screen` + `screen_mode=direct` → 成员间转发
    - `media=screen` + `screen_mode=relay` → **SFU 视频中继**（§4.3，服务端是 offerer）
  - `target` 仍接受（历史字段）但**只做一致性校验，不再参与路由**：与策略矛盾就报
    `INVALID_ARGUMENT`（例如 `voice_mode=direct` 却带 `target:"sfu"`），避免「客户端以为走中继、
    实际被当直连转发」这种静默错配。
  - 为什么必须显式 `media`：语音与屏幕共享各用**独立 PeerConnection** 之后，原生侧必须把每条
    offer/answer/candidate 精确投递到对应会话。`candidate` 的信封里只有 `mid`/`candidate` 文本，
    **不带媒体类型**；靠 SDP 嗅探只能「两个会话各试一次、让 libwebrtc 拒掉一个」——那是静默失败。
    客户端本来就知道信号属于哪个会话（媒体核心按会话发 `pu_signal_event`），直接标上去即可。
  - `kind ∈ { offer, answer, candidate }`
  - `payload` 是**媒体核心产出的原始信令信封字符串**（`{"type":"offer","sdp":"…"}` /
    `{"type":"answer","sdp":"…"}` / `{"type":"candidate","mid":"0","mline":0,"candidate":"…"}`）。
    服务端当不透明字符串转发，长度上限 64 KiB。
  - `to` 省略 = 广播给频道内其它订阅者（offer 用这条）；`to` 指定成员 = 定向投递
    （answer / candidate 用这条，观看者只回给共享方，不打扰其他观看者）。
- 服务端下发 `media.signal { channel_id, from, from_name, media, kind, payload }`，
  接收方按 `media` 把 payload 交给对应会话：
  `pu_signal_in_media(engine, "voice", payload, len)` 或 `pu_signal_in_media(engine, "screen", payload, len)`
  （ABI v2 加法式扩张，第 20 个导出；**不要回退用 `pu_signal_in`**，那会让路由重新变得不可诊断）。
  接收方据此决定 answer/candidate 发给谁。

2026-11-02 起 `media.signal` 就是屏幕共享直连的实际通道。原设计稿里的
`p2p.start` / `p2p.signal { to, channel_id, media, kind, payload }`（`kind ∈ { offer, answer, ice }`）
调整为：`kind` 与媒体核心 `pu_signal_event.kind` 对齐（`ice` → `candidate`），
`p2p.start` 暂不实现（观看方收到 offer 即自动应答）。

服务端角色：成员准入、消息定向、超时清理。它不解析也不改写 SDP——一旦开始 munge，直连就变成了"半个 SFU"。

### 4.4 直连失败上报与回落（`voice.transport_failed`）

direct 策略下，客户端检测到本端 ICE **连接失败**（`ice_state == FAILED`）或建会话后
**N 秒内未连通**（默认 15 秒）时，发一条**只含事实**的消息：

```json
voice.transport_failed { "channel_id": "...", "media": "voice", "reason": "ice_failed" }
```

`reason ∈ { ice_failed, ice_timeout }`。服务端收到后**自己决定**是否把该频道的 voice
transport 降级为 relay，并广播新的 `voice.state`（`voice_mode` 变为 relay），所有成员按
新 transport 重建会话。

**语义是「告知」不是「请求」**——客户端说的是「我这边连不上」，**不是**「给我 relay」。
客户端**不得**据此推断自己可以指定 transport：路径决定权始终在服务端
（PRODUCT 传输策略第 3 条约束）。后来人若想把它当 override 用，先回头读这一句。

落地规则：

- **按频道降级，不按成员**：`PRODUCT.md` 非目标明确「不做逐成员混合路径」。direct v1 只
  支持 2 人，一对人连不上就等于这条通话连不上，按频道降级是唯一自洽做法。
- **单向**：本进程生命周期内降级后不再升回 direct（避免抖动）；重启服务端才恢复策略原值。
- **幂等**：同一成员重复上报不重复降级。
- **防滥用**：同一成员 N 秒内最多触发一次降级判定（否则网络抖动会刷屏）。
- **日志**：服务端必须打印
  `voice: transport downgraded direct→relay channel=… reason=… member=…`。
- 客户端写一条路径历史：`直连 → 中继（直连超时/失败，已自动回落）`，
  UI 只显示这条记录 + 一句轻提示，**不重进频道、不掉线**。

与 §6 的关系：§6 的 `media.report` / `media.path` 是「路径状态可视化」；
`voice.transport_failed` 是「触发服务端决策的事实输入」，两者不互相替代。

## 5. 屏幕共享

- `screen.start { channel_id, source: { id, title, width, height, kind }, mode? }`
  → `screen.start.ok { channel_id }`，随后服务端广播 `screen.state`
  - `source.kind ∈ { screen, window, synthetic }`；`id` 是媒体核心给的源 id
    （显示器为 HMONITOR 的十进制串），`width/height` 是源分辨率
  - **传输方式由服务端 `--screen-mode` 决定，与 `--voice-mode` 无关**；请求里的 `mode` 只作告知、
    不参与决策（客户端传什么都不影响结果）。`screen.state` 里回显服务端实际值。
  - `screen_mode=relay` 时，`screen.start` 走服务端 SFU 视频中继（§4.3）：
    **本端不建自己的屏幕 PeerConnection**，而是在服务端下发的视频 PC 上把采集轨挂到
    recvonly 视频 m-line（`pu_screen_start_ex(..., flags&1)`，被动方角色）。
    若原生侧不支持被动屏幕会话，必须在**采集开始前**失败并告知用户，
    绝不允许「先开采集、再把视频轨挂到语音 PC 上」——历史实测 `role mismatch` → PCM 冻结。
- `screen.stop { channel_id }` → `screen.stop.ok { channel_id }`，广播 `screen.state`
- 广播 `screen.state { channel_id, mode, shares: [{ sharer_id, member_name, source, mode, viewers: [member_id] }] }`
  —— `mode` 是服务端屏幕共享策略（`relay|direct`），任何订阅者都能在加入共享/语音之前知道；
  没有共享时 `shares` 是 `[]`，不推 `null`
- **订阅时的初始快照（2026-11-06 补入）**：`channel.subscribe` 成功后，如果该频道已有共享，
  服务端只向新订阅者补发一次当前 `screen.state`。于是「甲已在共享 → 乙晚加入」不需要甲再操作，
  乙立刻能看到；没有共享时不发空快照（客户端默认空列表，策略值由 `voice.state.screen_mode`
  提供），避免给既有客户端增加无意义事件。

### 5.1 显式单流观看（`screen.watch` / `screen.unwatch`，v1.1）

**opt-in**：只有 auth 时声明了 `screen.watch.v1` 的连接才能调用；未声明连接保持 v1 行为
（订阅后自动绑定所有共享）。服务端在 `auth.ok.server.capabilities` 广告能力，双方都显式。

- `screen.watch { channel_id, sharer_id }` →
  `screen.watch.ok { channel_id, sharer_id, slot, generation, transport }`
  - 目标必须是本频道正在共享的其他成员；自己、未订阅、无共享分别返回
    `INVALID_ARGUMENT` / `FORBIDDEN` / `NOT_FOUND`；
  - `transport` 由服务端 `--screen-mode` 裁决，客户端不能选；
  - `relay`：`slot` 是本次观看者 PC 上绑定的 sendonly 槽（0/1，和上一目标不同），
    服务端先解绑旧目标再绑新目标；`generation` 每频道每观看者单调递增；
  - `direct`：`slot = -1`，服务端只做选台与 1v1 容量校验（已有其他观看者返回 `CONFLICT`），
    不建 SFU 槽、不假装能停对端 RTP；
  - 原生随后调用 `pu_screen_watch_ex(sharer_id, slot, generation)`：
    按「目标 + 槽位 + 代次」过滤帧，切源后旧目标的迟到帧被丢弃。
- `screen.unwatch { channel_id, sharer_id?, receive_id? }` → `screen.unwatch.ok { channel_id, sharer_id }`
  - 省略 `receive_id` 时是 legacy 目标语义：幂等；`sharer_id` 与当前目标不一致时不清新目标
    （返回空 `sharer_id`）；
  - 带 `receive_id` 时是 §5.1.1 的严格会话语义（只清当前连接持有且会话完全匹配的 lease）；
  - relay：服务端解绑该观看者的槽/关闭对应接收 PC，网络层停止转发；
  - direct：服务端不介入 P2P，停收由原生 `pu_screen_unwatch` 解绑 sink/解码完成
    （**传输层 UDP 是否立刻停收不在客户端可控范围**，见 §5.1 限制）。
- 旧服务端不认识这两条消息，会返回错误；旧 DLL 的 `pu_screen_watch/unwatch` 导出存在但
  返回 `PU_NOT_IMPLEMENTED`，因此客户端必须用 `pu_engine_caps` 能力位判断，不能按导出存在判断。
- 一次只能看一路：切目标即先解绑旧槽/旧 sink；不存在多目标 mesh。

#### 5.1.1 严格接收传输（`screen.watch.isolated.v1`，B2）

声明了该能力的连接在 `screen.watch` 里**必须**带 `receive_id`（客户端生成的
`[A-Za-z0-9_-]{8,128}`，并在发请求前已用它建好原生 receive-only 会话）：

- `screen.watch { channel_id, sharer_id, receive_id }` →
  `screen.watch.ok { channel_id, sharer_id, slot:-1, generation, transport:"relay",
   session_id, nonce, isolated:true }`
  - 服务端为该次观看新建一条只含一路 sendonly video 的 PeerConnection（新
    ICE/DTLS/SSRC），`session_id` 原样回显 `receive_id`，`nonce` 由服务端生成并绑定
    `(连接, 目标, generation)`；offer 只推给该观看者的连接，并带同一
    `session_id`/`session_nonce`/`generation`。
  - 观看者的 `media.signal`（answer/candidate，`target:"sfu"`）必须带
    `session_id` + `session_nonce`；任一不匹配、lease 已关闭或来自另一条连接，
    服务端返回 `CONFLICT`，旧信令不会落进新接收器。
  - `screen.unwatch.ok` 返回时服务端已完成接收 PC 关闭并撤转发；不触碰发布 PC、
    发布轨、采集源或语音。发布停止会关闭所有指向它的接收会话。
  - **按会话精确撤租**：`screen.unwatch { channel_id, receive_id }` →
    `screen.unwatch.ok { channel_id, sharer_id }`
    - `receive_id` 是会话身份；`sharer_id` 不参与严格路径的判定（目标相同不代表同一
      会话，目标不同也不代表不是当前会话）。服务端只清「发起连接持有 + `session_id`
      完全匹配」的那条 lease；已关闭、已被新会话取代、属于同成员另一条连接或别的成员
      时，幂等成功并返回空 `sharer_id`，绝不清当前新 lease。
    - 撤销必须先于 ACK 可用：客户端在发 `screen.watch` 之前就持有 `receive_id`，
      Stop 先本地关收再按会话撤租；迟到的 `watch.ok` 之后客户端会再撤一次同一会话，
      因此服务端必须允许对未知/已关闭会话的幂等撤销。
    - 未声明 `screen.watch.isolated.v1` 的连接带 `receive_id` → `INVALID_ARGUMENT`
      （不静默按 legacy 处理）；非法 `receive_id` 同样 `INVALID_ARGUMENT`。
    - 省略 `receive_id` 时保持上面的 legacy `sharer_id` 目标语义（`CONFLICT` 不会
      因为严格 lease 存在而出现；旧客户端行为不变）。
  - `receive_id` 的会话生命周期（服务端仲裁，客户端必须按错误处理）：
    - `receive_id` 是 `directSessions` 的键；客户端生成的值不能假设跨 viewer /
      跨 socket 唯一。任何**仍存活**的会话占用同一 id（即使 viewer/目标相同）都
      返回 `CONFLICT`：不覆盖旧 lease、不改旧 watch；旧会话的 unwatch/信令继续
      只作用于旧会话，只有它真正关闭后同一个 id 才可复用；
    - 同一成员在同一频道同一时刻只允许一个 watch 在建：另一条连接并发 watch 会得到
      `CONFLICT`，不会出现「SFU 活着一个会话、hub 记录另一个」；
    - 换目标时先建好新接收传输再原子换入，旧传输随后关闭；创建失败保留旧 lease，
      不让观看者处于「两边都没有」。
  - `direct` 的严格接收（B2b，v2.17）见 §5.1.2；未声明共享者能力时的
    direct 严格 watch 明确返回 `NOT_IMPLEMENTED`，不静默降级。legacy direct
    1v1 的停看控制见 §4.3：服务端向共享者推 `media.signal
    kind=viewer_close|viewer_arm`（payload 为空），共享者只暂停/恢复该 viewer
    的发送轨，保留采集与发布状态。
- 旧服务端不广告本能力：客户端必须明确报不支持，不得退回旧混合 PC 的
  slot/sink-only 路径冒充成功。
- **隔离的判据是传输身份，不是「新帧停了」**：新会话有新 ICE ufrag/DTLS 指纹/
  SRTP 密钥；旧会话的 RTP 加密字节在新会话上无法通过 SRTP 认证，因而不会变成
  新会话的应用交付。`server/internal/sfu` 的真实 UDP/SRTP 回归
  （`video_receive_replay_test.go`）在 socket 层暂扣实际写出的旧密文，再把它从
  原 socket 投给新 endpoint，断言新会话零旧 payload、当期新媒体继续通过。
- `VideoRoom.Refresh`（v1 混合上传/观看 PC 重建）**不是**严格接收重建：它只在
  legacy 路径上重建该成员的视频 PC，不替换/关闭/改 nonce 严格接收会话；严格重建
  是 `CloseReceive` + 带新 `receive_id` 的 `OpenReceive`。两者不得互相替代。

#### 5.1.2 direct 严格接收（B2b，`screen.publish.sessions.v1`）

`screen_mode=direct` 下，共享者**没有** SFU 可回放，因此严格观看由共享者直接为
该 viewer 建一条独立发送传输；服务端只做会话仲裁与信令转发，不碰媒体。

**能力**

- 客户端（作为共享者）在 `auth.capabilities` 里声明 `screen.publish.sessions.v1`，
  表示它能为单个 viewer 建独立发送会话（新 PC、复用已有采集源/视频轨）。
  发布归属与能力都是**连接级**的：`viewer_arm` 只发给真正发起 `screen.start`
  的 `shareOwners` 连接；同成员的另一条 socket 即使声明了该能力，也因为没有这路
  共享的采集源/视频轨而不能补位（`NOT_IMPLEMENTED`，不会把 arm 发错窗口）。
- 服务端在 direct（以及 relay+SFU）模式广告 `screen.watch.isolated.v1`。
- 共享者未声明该能力时，`screen.watch{receive_id}` 返回 `NOT_IMPLEMENTED`
  （`direct` 严格接收不可用），绝不退回 `pause_send` 冒充隔离。owner 断线或会话
  被撤销时按原连接清理，不把旧会话迁移到同成员其它 socket。

**会话建立**

1. 观看者先用自己的 `receive_id` 建好原生 receive-only 会话，再发
   `screen.watch { channel_id, sharer_id, receive_id }`。
2. 服务端原子提交一条 direct 会话：`(viewer 连接, sharer 发布连接, session=receive_id,
   服务端 nonce, generation)`；同一成员/同一目标仍是 1v1 容量（换目标先建新会话
   再换入，旧会话随后关闭）。
3. 服务端给共享者推 `media.signal kind=viewer_arm`，带 `session_id`、`session_nonce`、
   `generation`、`from`=观看者 id（**非空 session 字段 = 严格会话；空 = legacy
   暂停/恢复**）。
4. 共享者按 arm 建 per-viewer 发送会话（新 ICE/DTLS/SSRC），把原生产生的 offer
   通过 `media.signal { kind:"offer", to:<viewer>, media:"screen", session_id,
   session_nonce, generation }` 发给该观看者（不带 `target`，服务端按会话转发）。
5. 观看者把 offer 灌进自己的 receive 会话，回
   `media.signal { kind:"answer", to:<sharer>, media:"screen", session_id,
   session_nonce, generation }`；共享者再灌进对应发布会话。candidate 同路径。
6. 服务端校验：会话存在且未关闭、发送者正是会话双方之一且**连接**就是该端连接、
   nonce 常量时间比较、`generation` 必须与 arm 时**精确相等**（严格会话不接受
   缺省 0 冒充）、方向（共享者只能 offer/candidate，观看者只能 answer/candidate）。
   任一不满足返回 `CONFLICT`，不投递。`generation` 由 `screen.watch.ok` 下发，
   后续 offer/answer/candidate 必须原样带回（旧的无 session_id 客户端不进入本路径）。

**结束与隔离语义**

- 观看者停看：`screen.unwatch` 先本地关接收传输，再按 `receive_id` 精确撤租；
  服务端随后给共享者推 `viewer_close`（带 `session_id`），共享者只关**该 viewer**
  的发送会话。采集源、视频轨、发布状态、其它 viewer 的会话、语音都不受影响。
- 共享者停共享/断线/会话被撤销：服务端给观看者推
  `media.signal kind="receive_close"`（带 `session_id`），观看者只关本地接收传输，
  **不再发撤租**（服务端已清理），同样不碰发布/采集/语音。
- 旧会话身份立即失效：会话关闭后其 `session_id` 的 offer/answer/candidate 一律
  `CONFLICT`；重看必须用新 `receive_id`（新会话、新 nonce、新传输）。
- 服务端仍裁决 1v1 容量与路径：direct 不允许多个观看者共用一条发送传输，
  也不改变 `--screen-mode` 语义；legacy（不带 `receive_id`）行为不变。

### 4.3 屏幕共享 SFU 视频中继（`screen_mode=relay`）

与语音中继（§4.1）同构，但**独立 PeerConnection**（`sfu.VideoRoom`）：客户端本来就把语音与屏幕
拆成两个会话，一个服务端 PC 无法被两个客户端 PC 应答；分开也让「停共享」与「语音」在结构上互不牵连。

- 每成员的视频 PC 预协商：**1 条 recvonly 视频 m-line**（上传自己的屏幕）
  + **2 条 sendonly 视频槽**（看别人的屏幕，占位轨 id `partyup-video-slot-00` / `-01`）。
  单频道同时共享上限 2 路（v1 规模目标；首轮验收只做 1 路）。
- 共享者不换 PC：往自己那条 recvonly 视频 m-line 上传；服务端 `OnTrack` 收到后
  `ReplaceTrack` 进其他成员的空槽（零重协商，与音频完全同构）。
- 观看者中途加入：offer 发出去之后服务端再把**已有共享**回填进它的空槽，
  因此「共享开始后才进来的人」也能看到画面。
- `screen.stop` 时服务端立刻 `ReplaceTrack(nil)` 清空该共享者占的槽位：
  否则观看者会一直收到最后一帧，看起来像画面卡死。
- **只有 `screen_mode=relay` 才会建视频 PC**。`screen_mode=direct` 下绝不能凭空多一条
  到服务端的视频连接——用户以为屏幕是 P2P，实际却连服务器，既是资源浪费也是隐私问题
  （`TestScreenDirectCreatesNoVideoPeerConnection` 锁定）。
- 客户端侧 ABI（加法式，待媒体核心提供）：
  - `pu_screen_start_ex(engine, source_id, flags)`，`flags` bit0 = 被动（answerer）：
    采屏 + 把视频轨挂到服务端 offer 的 recvonly m-line 上，**不生成 offer**；
  - `pu_screen_prepare(engine)`：观看者预建屏幕会话为 answerer，等 `pu_signal_in_media("screen", …)` 灌 offer；
  - 按视频槽取帧（多路共享）暂未定义，首轮只支持 1 路共享。
- 编解码：视频槽占位轨按 **H264** 协商（客户端媒体核心硬件优先）；VP8 回落在 SFU 视频首轮不支持。
- 码率/分辨率：服务端只转发不转码；v1 验收按 1080p 进行（`PRODUCT.md` 规模目标）。

信令走 §4.2 的 `media.signal`，**`media` 固定为 `"screen"`**：共享方 `screen.start` 后媒体核心
产出 offer，经 `media.signal media=screen kind=offer` 发出；观看方按媒体类型交给自己的
**屏幕会话**（`pu_signal_in_media(engine, "screen", …)`），应答的 answer 经
`media.signal media=screen kind=answer to=<sharer_id>` 回给共享方；candidate 同理。
屏幕共享与语音**各用独立 PeerConnection**，互不影响角色与协商状态（这正是 relay 下
「点共享把语音搞坏」的根因修复）。

`screen.watch` / `screen.unwatch` 已实现（v1.1，见 §5.1）：显式 opt-in 的客户端由服务端裁决
transport 并把 `slot`/`generation` 交给原生；未 opt-in 的旧客户端保持「订阅后自动观看」。
仍不闭环的边界：direct 的 unwatch 只解绑本地 sink/解码，**不保证对端 RTP 立刻停发**；
relay 切换目标时旧目标在途 RTP 可能短暂到达，由原生按 target/epoch 丢弃，不冒充新目标。

## 6. 路径事件

服务端不知道直连是否成功，所以**路径状态由客户端上报**：

- 客户端：`media.report { channel_id, media, path, reason }`
- 服务端广播：`media.path { channel_id, member_id, media, path, reason, ts }`
- `path ∈ { connecting, direct, relay, failed }`
- `reason ∈ { joined, ice_failed, no_media, user_forced, timeout }`

这条链路是产品差异点：任何人都能看见"谁现在走的是中继"。上报频率限制为每条流每 2 秒最多一次，服务端做去重。

## 7. 在线状态与全服务器名册

### 7.1 频道 presence（兼容保留）

`presence { channel_id, members: [{ member_id, name, display_name, state }] }`，
`state ∈ { online, voice, sharing }`。这是**频道范围**列表：该频道当前订阅者按 `member_id`
去重后的成员。

`name` 仍是唯一登录名，`display_name` 是展示名（2026-11-06 补入），两者同时下发。
v1 服务端只发 `online`；「谁在共享」由 `screen.state` 单独推送，不要从 presence 推断。
按订阅推送，变化即发，同一频道 200ms 内合并。旧客户端忽略新增的 `display_name` 即可。

### 7.2 全服务器名册（2026-11-06 补入）

认证后可取全服务器名册，包含**离线成员**：

- `member.list`（WS，空载荷）→ `member.list.ok { members: [...] }`
- `GET /api/members`（`Authorization: Bearer <token>`）→ `{ "members": [...] }`（同一形状）
- 推送 `members.state { members: [...] }`：在成员上线/离线、进入/离开语音时发给**所有已鉴权连接**
  （不依赖频道订阅）。这是名册的增量路径；频道内 `presence` 语义保持不变。

元素形状：`{ id, name, display_name, admin, online, state, voice_channel_ids }`

- `online`：按 `member_id` 聚合该成员的**所有已鉴权 WebSocket**。同一成员多连接仍是名册里的一项；
  其中一条断开不会让成员变离线。
- `state`：`offline` | `idle` | `voice`。`idle` = 在线但**不在任何语音频道**
  （空闲不是「没有键盘活动」）；`voice` = 在线且至少在一个语音频道。
- `voice_channel_ids`：该成员当前实际所在的**全部**语音频道 id，按字典序。它可能多于一项
  （同一成员通过不同连接分别进入不同频道是合法状态），因此协议用数组，不能静默取一个频道
  冒充唯一位置。**文字频道订阅是另一套状态，不能拿来推断语音位置**，也不会出现在该字段里。
- 不包含密码哈希、会话 token 或其他敏感字段。

客户端可用 `state` 分三组展示：`offline` / `idle`（在线空闲）/ `voice`（在语音；再按
`voice_channel_ids` 与当前频道区分「本频道」与「其他频道」）。

`auth.ok.server.capabilities` 在支持本能力时包含 `member_roster`；旧客户端可忽略。

### 7.3 匿名服务器探测（`GET /api/server-info`，2026-11-07 补入）

客户端「先添加服务器」流程用，**不需要 token**。响应：

```json
{ "name": "PartyUp", "version": "1.0.0", "protocol_version": 1,
  "initialized": true, "registration": "invite",
  "voice_mode": "relay", "screen_mode": "relay",
  "bootstrap_allowed": false }
```

- `initialized`：库里是否已有成员（**每次请求重新查库**，不做进程内缓存，避免 bootstrap
  完成后还要重启才更新）；
- `registration`：`invite`（默认，必须邀请码）| `closed`（关闭注册）| `open`（无需邀请码直接注册，
  2026-11-07 补）；未知值按 `invite` 处理；
- `bootstrap_allowed`：**只是 UI 提示**，表示「这条连接 + 当前状态」是否值得尝试 bootstrap。
  真正授权在 `POST /api/bootstrap` 内部（§7.4）。
- 该端点与登录共用限速器（按 IP），防止公网扫状态/爆破 bootstrap token。

### 7.4 bootstrap 安全边界（2026-11-07 补入）

bootstrap 会创建全服唯一的 owner，因此除「一次性 token + 24h TTL」外再加**连接可信**检查：

1. 连接来自**回环地址**，或
2. 连接是 **TLS**（`r.TLS != nil`，即服务端直接终止 TLS），或
3. 部署者显式开启 **`--trust-proxy`**（声明反向代理已终止 TLS，此时才信任
   `X-Forwarded-For` 作为客户端地址）。

以上都不满足 → `403 FORBIDDEN`，并在服务端日志记一条 `bootstrap rejected` 及原因
（含 `remote` 与 `tls` 字段）。已初始化后再次 bootstrap → `409 ALREADY_BOOTSTRAPPED`；
库里没有可用 bootstrap token 时 → `409 ALREADY_BOOTSTRAPPED`（需要重启生成新 token 的场景除外）。

bootstrap 失败计入登录限速器（成功则清零），防止爆破 token。**公网部署必须**：
用反向代理终止 TLS 并开 `--trust-proxy`，或先本机 bootstrap 再对外开放端口。

## 本地音频监测 ABI 补充（v2.18，非网络消息）

`pu_audio_monitor_start(engine)`、`pu_audio_monitor_stop(engine)` 与
`pu_audio_monitor_status(engine, info*)` 是本地 C ABI，不新增 HTTP/WS 消息、
服务端 capability 或语音订阅，也不消耗频道容量。native `pu_engine_caps` bit7
与服务端 `auth.ok.server.capabilities` 是不同层的能力，不能混用。

`pu_audio_monitor_info` 为 40 字节：`struct_size`（u32）、`state`（i32：
0 Idle / 1 Active / 2 Faulted）、`running`（i32：-1 未知 / 0 未运行 / 1 运行）、
`valid/peak/rms/frames/reserved`（各 u32）、`last_frame_ms`（u64，单调时钟毫秒）。
`last_frame_ms` 偏移 32，`reserved=0`。state 与最近读回的 running 分开，
Faulted 不代表已经停录；Stop 无输出参数，状态缓冲无效不能阻止停止请求。

只有显式 Start 才请求采集，同一 ADM 输入与语音排他，不建 PC/音轨、不发送信令或媒体；
Stop 失败保留真实/未知运行状态。PCM 电平包含有效全零帧；一秒内无新帧、未启用或停止后
不能把旧峰值当有效数据。查询不重启采集，不自动解除手动静音；旧 DLL 缺能力位或任一
导出时明确不支持，不合成电平。仅本地 fake/SDK 合成回归已验证，真实硬件、全局 PTT、
旧 DLL 运行矩阵与正式 UI 接线仍未验。完整约束见[输入测试合同](<design/v1.1-aud02-contract.md>)。

## 8. 待定

- 断线重连语义：语音会话是否在 30 秒内保持（倾向保持，避免"地铁过隧道就掉线"）。
- 是否允许服务端改写 SDP 以适配 SFU（倾向不允许，改写只发生在媒体核心内部）。
- 心跳失败与半开连接的判定细节。
- 文件/图片消息（v1 不做，但 `text` 的 payload 要能扩展）。

## 9. 服务器管理 API（2026-11-07 补入）

全部端点要求 `Authorization: Bearer <token>`，**角色在服务端强制校验**（§1.5）。响应体统一为
JSON；失败统一 `{ "code", "message" }`。`{id}` 是成员 id（ULID）。

| 方法 | 路径 | 最低角色 | 说明 |
| --- | --- | --- | --- |
| GET | `/api/admin/overview` | admin | 概览：成员/在线/频道/语音房间/共享路数/模式/注册策略 |
| GET | `/api/admin/settings` | admin | 取设置快照（同 `settings.state`） |
| PATCH | `/api/admin/settings` | admin | 字段白名单合并；危险项仅 owner（否则 `OWNER_ONLY`） |
| GET | `/api/admin/members` | admin | 成员列表（含 `role`、`banned`、`banned_at`、`ban_reason`） |
| POST | `/api/admin/members/{id}/kick` | admin | 撤销该成员全部会话并关闭其 WS（owner 拒绝） |
| POST | `/api/admin/members/{id}/ban` | admin | 永久封禁：撤销会话 + 关连接；body `{ "reason"? }` |
| POST | `/api/admin/members/{id}/unban` | admin | 解封（幂等） |
| POST | `/api/admin/members/{id}/password` | admin | 重置密码：生成临时密码**只返回一次**并撤销该成员会话 |
| POST | `/api/admin/members/{id}/role` | **owner** | body `{ "role": "user" \| "admin" }`（不能设 owner、不能动 owner） |
| POST | `/api/admin/owner/transfer` | **owner** | body `{ "member_id" }`；旧 owner 自动降 admin |
| GET | `/api/admin/invites` | admin | 邀请码列表（含创建者/使用者/是否已用） |
| POST | `/api/admin/invites` | admin | 生成邀请码；响应 `{ "invite": { code, created_at, ... } }` |
| DELETE | `/api/admin/invites?code=...` | admin | 吊销（已使用的码也可删，历史靠审计） |
| GET | `/api/admin/channels` | admin | 频道列表（id/name/description/position） |
| POST | `/api/admin/channels` | admin | body `{ "name", "description"? }`；重名 409 |
| PATCH | `/api/admin/channels` | admin | body `{ "id", "name"?, "description"? }`；或 `{ "order": [id...] }` 重排 |
| DELETE | `/api/admin/channels?id=...` | admin | 仅空频道可删（有订阅/语音/共享 → 409） |
| POST | `/api/admin/messages/delete` | admin | body `{ "channel_id", "message_id" }` |
| GET | `/api/admin/audit?limit=&before=` | admin | 审计事件，按 id 倒序；`before` 传上一页最小 id 翻页 |

**封禁语义（§5.1）**：ban 立即撤销该成员**全部会话**并关闭其在线连接；此后登录、`/api/me`、
`/api/members`、WS 鉴权一律 `403 MEMBER_BANNED`（登录路径在密码校验**之后**才检查封禁，
避免泄露账号状态）。owner 永不可被封禁/踢出/降级。解封不自动恢复旧 token（旧会话已撤销，需重新登录）。

**审计（§4）**：每次管理写操作在**业务事务提交之后**追加一条 `audit_events`
（`actor_id`/`actor_name` 快照、`action`、`target`、`detail` JSON、`ip`）。写审计失败只记 warn，
不影响主操作。覆盖动作：`member.kick` / `member.ban` / `member.unban` / `member.rename` /
`member.reset_password` / `member.role_change` / `owner.transfer` / `channel.create` /
`channel.rename` / `channel.delete` / `channel.reorder` / `message.delete` / `invite.create` /
`invite.revoke` / `settings.update` / `server.mode_change`。`detail` 永不含密码或 token。

**权限矩阵（实测值见验收证据）**：

| 操作 | owner | admin | user |
| --- | --- | --- | --- |
| 看管理概览/成员/审计/邀请/频道 | ✅ | ✅ | ❌ `ADMIN_ONLY` |
| 踢/封/解封/重置密码（非 owner 目标） | ✅ | ✅ | ❌ |
| 任免管理员（user ↔ admin） | ✅ | ❌ `OWNER_ONLY` | ❌ |
| 改危险设置（voice/screen_mode、max_screen_shares） | ✅ | ❌ `OWNER_ONLY` | ❌ |
| 改普通设置/频道/邀请 | ✅ | ✅ | ❌ |
| 转让所有权 | ✅ | ❌ | ❌ |
| 任何针对 owner 的写操作 | —（owner 不可被管理） | ❌ `OWNER_ONLY` | ❌ |
| 改自己的显示名/密码 | ✅ | ✅ | ✅ |
