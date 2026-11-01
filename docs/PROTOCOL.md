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

错误码（第一批）：`UNAUTHENTICATED` `FORBIDDEN` `NOT_FOUND` `CONFLICT` `RATE_LIMITED` `INTERNAL`。

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

## 2. 频道

- `channel.list` → `channel.list.ok { channels: [{ id, name, voice: [...], shares: [...] }] }`
- `channel.subscribe { channel_id }` → 之后该频道的 `text.new` / `presence` / `media.*` 推给本连接
- `channel.unsubscribe { channel_id }`

订阅是显式的：不订阅就不推送，避免大服务器把无关频道的消息刷给所有人。

## 3. 文字

- `text.send { channel_id, client_msg_id, body }` → `text.send.ok { id, seq, ts }`
- 广播 `text.new { channel_id, id, seq, member_id, body, ts }`
- `text.history { channel_id, before_seq, limit }` → `text.history.ok { messages: [...] }`

`seq` 是**每频道单调递增的整数**，由服务端分配；客户端按 `seq` 排序，不依赖时间戳。`client_msg_id` 用于本地乐观显示与去重。

## 4. 语音

- `voice.join { channel_id, mode }`，`mode ∈ { relay, direct }`
- `voice.leave { channel_id }`
- `voice.set_mode { channel_id, mode }` —— 只允许整频道切换
- 广播 `voice.state { channel_id, mode, members: [{ member_id, muted, deafened }] }`

### 4.1 中继

成员加入后服务端下发：

```json
{ "type": "media.offer", "payload": { "channel_id": "...", "media": "voice",
  "transport": "relay", "session_id": "...", "sdp": "v=0..." } }
```

客户端媒体核心 `setRemoteDescription` 后回 `media.answer { session_id, sdp }`。ICE 候选随 SDP 一起（trickle 后续再加）。

### 4.2 直连

服务端只做定向投递，不改 SDP 内容：

- `p2p.start { channel_id, media: "voice", peers: [member_id, ...] }` —— 服务端告诉每个成员该连谁
- `p2p.signal { to, channel_id, media, kind, payload }`，`kind ∈ { offer, answer, ice }`
- 收到方原样回 `p2p.signal` 给对方

服务端角色：成员准入、消息定向、超时清理。它不解析也不改写 SDP——一旦开始 munge，直连就变成了"半个 SFU"。

## 5. 屏幕共享

- `screen.start { channel_id, source: { kind: "screen" | "window", id, title }, mode }`
- `screen.stop { channel_id }`
- `screen.watch { channel_id, sharer_id }` / `screen.unwatch { channel_id, sharer_id }`
- 广播 `screen.state { channel_id, shares: [{ sharer_id, source, mode, viewers: [member_id] }] }`

每个观看者单独 `watch`，因为直连要为每个观看者单独建连接、单独编码——协议层面就把这个事实暴露出来，UI 才能如实显示"正在为 N 人分别编码"。

## 6. 路径事件

服务端不知道直连是否成功，所以**路径状态由客户端上报**：

- 客户端：`media.report { channel_id, media, path, reason }`
- 服务端广播：`media.path { channel_id, member_id, media, path, reason, ts }`
- `path ∈ { connecting, direct, relay, failed }`
- `reason ∈ { joined, ice_failed, no_media, user_forced, timeout }`

这条链路是产品差异点：任何人都能看见"谁现在走的是中继"。上报频率限制为每条流每 2 秒最多一次，服务端做去重。

## 7. 在线状态

`presence { channel_id, members: [{ member_id, name, state }] }`，`state ∈ { online, voice, sharing }`。

按订阅推送，变化即发，同一频道 200ms 内合并。

## 8. 待定

- 断线重连语义：语音会话是否在 30 秒内保持（倾向保持，避免"地铁过隧道就掉线"）。
- 是否允许服务端改写 SDP 以适配 SFU（倾向不允许，改写只发生在媒体核心内部）。
- 心跳失败与半开连接的判定细节。
- 文件/图片消息（v1 不做，但 `text` 的 payload 要能扩展）。
