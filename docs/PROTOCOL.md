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

**传输方式由服务端策略决定，客户端无权也无需选择。** 语音与屏幕共享是**两套独立策略**
（ADR-0002「三类媒体各自决定路径」）：

- `--voice-mode=relay|direct`（`PARTYUP_VOICE_MODE`，默认 `relay`）只管**语音**；
- `--screen-mode=relay|direct`（`PARTYUP_SCREEN_MODE`，默认 `direct`）只管**屏幕共享**。

两者可以不同：语音中继 + 屏幕直连是合法配置，反之亦然。客户端从
`voice.join.ok.transport`（语音）与 `voice.state.voice_mode` / `voice.state.screen_mode`
（两者）读取结果，再按结果走对应媒体路径。

> `--screen-mode` 默认 `direct` 是过渡值：SFU 视频通道尚未实现，默认配置下屏幕共享必须可用。
> 视频通道落地后默认值切到 `relay`（与语音对齐）。

- `voice.join { channel_id }` —— 上行载荷**只有** `channel_id`。
  客户端即使多传一个 `mode` 字段也会被服务端忽略（协议不再定义该字段）。
  响应 `voice.join.ok { channel_id, transport }`，`transport ∈ { relay, direct }`
  是服务端实际策略值。
- `voice.leave { channel_id }`
- `voice.set_mute { channel_id, muted }` / `voice.set_deafen { channel_id, deafened }`
  —— 上报本端静音/闭音状态。本地 ADM 状态仍由客户端自己控制；上报是为了让别人看见（`voice.state`）。
- 广播 `voice.state { channel_id, voice_mode, screen_mode,
  members: [{ member_id, muted, deafened }], slots?: [{ index, member_id, muted }] }`
  —— `voice_mode` / `screen_mode` 是两条独立策略的服务端实际值（`relay|direct`）；
  旧字段 `mode` **已删除**（v1 客户端同版本发布，无兼容负担）。
  `members` 按 `member_id` 排序，稳定可比对。
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

- `media.signal { channel_id, media, kind, payload, target?, to? }`
  - **`media` 必填，`media ∈ { voice, screen }`，且是路由的唯一依据**：
    - `media=voice` + `voice_mode=relay` → 交给服务端 SFU（§4.1，服务端是 offerer）
    - `media=voice` + `voice_mode=direct` → 成员间转发（offer 广播 / answer·candidate 定向）
    - `media=screen` + `screen_mode=direct` → 成员间转发
    - `media=screen` + `screen_mode=relay` → SFU 视频通道（**尚未实现**，返回 `NOT_IMPLEMENTED`）
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

## 5. 屏幕共享

- `screen.start { channel_id, source: { id, title, width, height, kind }, mode? }`
  → `screen.start.ok { channel_id }`，随后服务端广播 `screen.state`
  - `source.kind ∈ { screen, window, synthetic }`；`id` 是媒体核心给的源 id
    （显示器为 HMONITOR 的十进制串），`width/height` 是源分辨率
  - **传输方式由服务端 `--screen-mode` 决定，与 `--voice-mode` 无关**；请求里的 `mode` 只作告知、
    不参与决策（客户端传什么都不影响结果）。`screen.state` 里回显服务端实际值。
  - `screen_mode=relay` 且 SFU 视频通道未就绪时，`screen.start` **在广播 `screen.state` 之前**
    就返回 `NOT_IMPLEMENTED`（错误码），消息直接给用户看：
    「当前服务器为纯 SFU 中继模式，暂不支持屏幕共享（视频通道开发中）」。
    这是硬要求：不能在客户端先开采集再被拒——那会往已有的 PC 上挂视频轨，把语音一起拖坏
    （历史实测 `role mismatch` → PCM 冻结）。
- `screen.stop { channel_id }` → `screen.stop.ok { channel_id }`，广播 `screen.state`
- 广播 `screen.state { channel_id, mode, shares: [{ sharer_id, member_name, source, mode, viewers: [member_id] }] }`
  —— `mode` 是服务端屏幕共享策略（`relay|direct`），任何订阅者都能在加入共享/语音之前知道；
  没有共享时 `shares` 是 `[]`，不推 `null`

信令走 §4.2 的 `media.signal`，**`media` 固定为 `"screen"`**：共享方 `screen.start` 后媒体核心
产出 offer，经 `media.signal media=screen kind=offer` 发出；观看方按媒体类型交给自己的
**屏幕会话**（`pu_signal_in_media(engine, "screen", …)`），应答的 answer 经
`media.signal media=screen kind=answer to=<sharer_id>` 回给共享方；candidate 同理。
屏幕共享与语音**各用独立 PeerConnection**，互不影响角色与协商状态（这正是 relay 下
「点共享把语音搞坏」的根因修复）。

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
