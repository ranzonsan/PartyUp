# PartyUp 协议（草案 v0.1）

> 状态：草案。消息形状已定，字段名与错误码可能调整；实现前以本文为准，改协议先改本文。

## 0. 通用信封

WebSocket 承载全部控制消息，一条连接一个成员。

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

## 1. 握手与认证

1. 建立 WS → 客户端 30 秒内发 `auth`。
2. `auth { token }` → `auth.ok { member, server }`，否则 `auth.err` 后关闭连接。
3. 服务端每 20 秒发 `ping`，客户端回 `pong`；60 秒无 pong 断开。

HTTP 侧：

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| POST | `/api/bootstrap` | 首次启动、24 小时内用 stdout 打印的 bootstrap token 创建管理员 |
| POST | `/api/login` | 用户名 + 密码 → token |
| POST | `/api/invite/{code}` | 用邀请码注册并直接登录 |
| GET | `/api/me` | 校验 token、取当前成员信息 |

Token 是不透明随机串（32 字节，base64url），存在 SQLite 里、可撤销——不用 JWT，因为需要"踢下线"能力。

HTTP 请求/响应体形状（2026-11-02 落地，此前文档未定义）：

| 路径 | 请求体 | 成功响应体 |
| --- | --- | --- |
| `POST /api/bootstrap` | `{ "token", "username", "password" }` | `{ "member": { id, name, admin }, "token" }` |
| `POST /api/login` | `{ "username", "password" }` | 同上 |
| `POST /api/invite/{code}` | `{ "username", "password" }`（邀请码在路径里） | 同上 |
| `GET /api/me` | — | `{ "member": { id, name, admin } }` |

失败统一为 `{ "code": "<错误码>", "message": "<人类可读>" }`，状态码：400 参数非法（`INVALID_ARGUMENT`）、401 未认证（`UNAUTHENTICATED`）、403 禁止（`FORBIDDEN`）、404 不存在（`NOT_FOUND`）、409 冲突（`CONFLICT`）、500 内部（`INTERNAL`）。

## 2. 频道

- `channel.list` → `channel.list.ok { channels: [{ id, name, voice: [...], shares: [...] }] }`
- `channel.create { name }` → `channel.create.ok { channel: { id, name, voice: [], shares: [] } }`
  —— 2026-11-02 补入（原文档只有 list/subscribe/unsubscribe，没有创建入口）。任何已认证成员都可创建；
  `name` 为 1–32 个可见字符，**全局唯一**，撞名回 `CONFLICT`。创建后仍需 `channel.subscribe` 才会收到推送。
  `voice` / `shares` 在 v1 恒为 `[]`（空数组，**不是 `null`**）；共享状态以 `screen.state` 推送为准。
- `channel.subscribe { channel_id }` → 之后该频道的 `text.new` / `presence` / `screen.state` / `media.signal` 推给本连接
- `channel.unsubscribe { channel_id }`

订阅是显式的：不订阅就不推送，避免大服务器把无关频道的消息刷给所有人。

## 3. 文字

- `text.send { channel_id, client_msg_id, body }` → `text.send.ok { id, seq, ts, client_msg_id }`
- 广播 `text.new { channel_id, id, seq, member_id, member_name, body, ts }`
- `text.history { channel_id, before_seq, limit }` → `text.history.ok { messages: [...] }`（元素与 `text.new` 同形）

`seq` 是**每频道单调递增的整数**，由服务端分配；客户端按 `seq` 排序，不依赖时间戳。`client_msg_id` 用于本地乐观显示与去重（服务端在 `.ok` 里原样带回）。

`member_name` 是 2026-11-02 补入的：原来只有 `member_id`，历史消息里的离线成员无法显示名字（客户端只能退化到 id 前缀）。服务端在 `text.new` 与 `text.history` 里都填；`member_id` 保留，用于身份关联。

## 4. 语音

**传输方式由服务端策略决定，客户端无权也无需选择。** 服务端启动参数
`--voice-mode=relay|direct`（环境变量 `PARTYUP_VOICE_MODE`，默认 `relay`，见 ADR-0002）
决定语音走服务端中继还是 P2P 直连。客户端只从 `voice.join.ok.transport` 与
`voice.state.mode` 读取结果，再按结果走对应媒体路径。

- `voice.join { channel_id }` —— 上行载荷**只有** `channel_id`。
  客户端即使多传一个 `mode` 字段也会被服务端忽略（协议不再定义该字段）。
  响应 `voice.join.ok { channel_id, transport }`，`transport ∈ { relay, direct }`
  是服务端实际策略值。
- `voice.leave { channel_id }`
- `voice.set_mute { channel_id, muted }` / `voice.set_deafen { channel_id, deafened }`
  —— 上报本端静音/闭音状态。本地 ADM 状态仍由客户端自己控制；上报是为了让别人看见（`voice.state`）。
- 广播 `voice.state { channel_id, mode, members: [{ member_id, muted, deafened }] }`
  —— `mode` 就是服务端策略值；`members` 按 `member_id` 排序，稳定可比对。
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
- 槽位 12 ⇒ 中继语音每频道最多 **13 人**（12 个远端 + 自己）。超过时新发布者分配槽位失败
  会打 warn 并跳过那一路（该成员对那部分人不可闻），不会崩。
- 中继模式下**客户端不要给服务端发 offer**；收到 `kind=offer` 应理解成「服务端在协商」，只回 answer。

服务端实现见 `server/internal/sfu/room.go`（房间）与 `server/internal/server/voice.go`（协议接线），
端到端测试见 `server/internal/server/voice_relay_test.go`（2 人 / 3 人中继 + voice.state +
「服务端策略覆盖客户端意见」）。

### 4.1.1 direct 策略下

服务端**不发 offer**，只做 `media.signal` 转发（§4.2）：由成员之间自行 offer/answer
（1:1 直连）。服务端仍维护 `voice.state`（谁在语音、谁静音），并拒绝 `voice.set_mode`。

### 4.2 直连

服务端只做中转与定向投递，不解析也不改写 SDP：

- `media.signal { channel_id, target?, to?, media, kind, payload }`
  - `target` 省略 = 投递给频道成员（本节，direct 模式）；`target: "sfu"` = 交给服务端 SFU（§4.1，relay 模式）。
    relay 下 `to` 被忽略；direct 下 `target` 必须为空。
  - `media ∈ { screen, voice }`；`kind ∈ { offer, answer, candidate }`
  - `payload` 是**媒体核心产出的原始信令信封字符串**（`{"type":"offer","sdp":"…"}` /
    `{"type":"answer","sdp":"…"}` / `{"type":"candidate","mid":"0","mline":0,"candidate":"…"}`）。
    服务端当不透明字符串转发，长度上限 64 KiB。
  - `to` 省略 = 广播给频道内其它订阅者（offer 用这条）；`to` 指定成员 = 定向投递
    （answer / candidate 用这条，观看者只回给共享方，不打扰其他观看者）。
- 服务端下发 `media.signal { channel_id, from, from_name, media, kind, payload }`，
  接收方据此决定 answer/candidate 发给谁。

2026-11-02 起 `media.signal` 就是屏幕共享直连的实际通道。原设计稿里的
`p2p.start` / `p2p.signal { to, channel_id, media, kind, payload }`（`kind ∈ { offer, answer, ice }`）
调整为：`kind` 与媒体核心 `pu_signal_event.kind` 对齐（`ice` → `candidate`），
`p2p.start` 暂不实现（观看方收到 offer 即自动应答）。

服务端角色：成员准入、消息定向、超时清理。它不解析也不改写 SDP——一旦开始 munge，直连就变成了"半个 SFU"。

## 5. 屏幕共享

- `screen.start { channel_id, source: { id, title, width, height, kind }, mode? }`
  → `screen.start.ok { channel_id }`，随后服务端广播 `screen.state`
  - `source.kind ∈ { screen, window, synthetic }`；`id` 是媒体核心给的源 id
    （显示器为 HMONITOR 的十进制串），`width/height` 是源分辨率
  - `mode ∈ { direct, relay }`，缺省 `direct`；当前只实现 direct（P2P），relay 留给 SFU
- `screen.stop { channel_id }` → `screen.stop.ok { channel_id }`，广播 `screen.state`
- 广播 `screen.state { channel_id, shares: [{ sharer_id, member_name, source, mode, viewers: [member_id] }] }`
  —— 没有共享时 `shares` 是 `[]`，不推 `null`

信令走 §4.2 的 `media.signal`：共享方 `screen.start` 后媒体核心产出 offer，经
`media.signal kind=offer` 广播；观看方把 `payload` 原样喂给自己的媒体核心，应答的
answer 经 `media.signal kind=answer to=<sharer_id>` 回给共享方；candidate 同理。

`screen.watch` / `screen.unwatch`（每个观看者显式 opt-in，用于"为 N 人分别编码"的如实展示）
在 v1 暂不实现：媒体核心的 `pu_screen_watch` 目前返回 `PU_NOT_IMPLEMENTED`，观看方收到
offer 即自动协商。等 SFU 中继与逐观看者编码落地后再补这两条。

## 6. 路径事件

服务端不知道直连是否成功，所以**路径状态由客户端上报**：

- 客户端：`media.report { channel_id, media, path, reason }`
- 服务端广播：`media.path { channel_id, member_id, media, path, reason, ts }`
- `path ∈ { connecting, direct, relay, failed }`
- `reason ∈ { joined, ice_failed, no_media, user_forced, timeout }`

这条链路是产品差异点：任何人都能看见"谁现在走的是中继"。上报频率限制为每条流每 2 秒最多一次，服务端做去重。

## 7. 在线状态

`presence { channel_id, members: [{ member_id, name, state }] }`，`state ∈ { online, voice, sharing }`。

v1 只发 `online`：语音未实现，`voice` / `sharing` 两个状态等语音落地后再启用；「谁在共享」由 `screen.state` 单独推送，不要从 presence 推断。

按订阅推送，变化即发，同一频道 200ms 内合并。

## 8. 待定

- 断线重连语义：语音会话是否在 30 秒内保持（倾向保持，避免"地铁过隧道就掉线"）。
- 是否允许服务端改写 SDP 以适配 SFU（倾向不允许，改写只发生在媒体核心内部）。
- 心跳失败与半开连接的判定细节。
- 文件/图片消息（v1 不做，但 `text` 的 payload 要能扩展）。
