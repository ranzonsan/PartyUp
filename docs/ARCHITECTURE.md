# PartyUp 架构

> 状态：v0.1 · 2026-11-01。技术栈见 [ADR-0006](adr/0006-client-stack.md)；协议消息见 [PROTOCOL.md](PROTOCOL.md)（草案）。

## 1. 组件

三个交付物，两种语言边界：

| 组件 | 语言 / 运行时 | 职责 |
| --- | --- | --- |
| `partyup` | Go | HTTP API、WebSocket 信令、文字消息与历史、SFU 转发、SQLite 存储 |
| `partyup-ui` | C# / .NET 10 + Avalonia | 频道列表、文字区、成员与状态、语音/共享控制、路径指示 |
| `partyup-media` | C++ + libwebrtc（动态库） | 采集、编码、ICE/DTLS、抖动缓冲、解码、输出渲染帧 |

客户端进程 = `partyup-ui.exe` + `partyup-media.dll`。媒体核心与 UI 同进程：帧句柄跨进程传递要做共享纹理与生命周期管理，比进程内 C ABI 贵一个量级。

## 2. 三条边界

### 2.1 UI ↔ 媒体核心：C ABI

纯 C、无异常穿越、错误码返回。第一版只需要这些：

```c
typedef struct pu_engine pu_engine;

pu_engine* pu_engine_create(const pu_engine_config* cfg);
void       pu_engine_destroy(pu_engine* e);

// 语音
int  pu_voice_join(pu_engine* e, const pu_voice_params* p);   // 含 channel_id 与 mode
int  pu_voice_leave(pu_engine* e);
int  pu_voice_set_mode(pu_engine* e, pu_mode mode);           // relay | direct
int  pu_voice_set_mute(pu_engine* e, int muted);
int  pu_voice_set_deafen(pu_engine* e, int deafened);

// 屏幕共享
int  pu_screen_sources(pu_engine* e, pu_source_list* out);    // 枚举可共享的窗口/屏幕
int  pu_screen_start(pu_engine* e, const pu_screen_params* p);
int  pu_screen_stop(pu_engine* e);
int  pu_screen_watch(pu_engine* e, const char* sharer_id);
int  pu_screen_unwatch(pu_engine* e, const char* sharer_id);

// 渲染：UI 每帧调用，拿回需要绘制的内容
int  pu_frame_acquire(pu_engine* e, const char* stream_id, pu_frame* out); // 纹理句柄或 BGRA 指针
void pu_frame_release(pu_engine* e, pu_frame* f);

// 事件：轮询式，UI 定时 drain
int  pu_poll_event(pu_engine* e, pu_event* out);              // 路径变化、统计、错误、成员状态
```

约定：所有返回 `int` 的函数用负值表示错误码；字符串为 UTF-8、以 0 结尾；`pu_frame` 由引擎持有，`release` 前不可跨帧复用。

### 2.2 UI ↔ 服务端：HTTP + WebSocket

- HTTP：登录、邀请码兑换、频道列表、历史消息分页。
- WebSocket：一条长连接承载信令、文字消息、在线状态、路径事件。
- 连接与鉴权见 [PROTOCOL.md](PROTOCOL.md)。

UI 不直接处理 SDP/ICE；它把服务端下发的媒体指令原样转给媒体核心（`pu_*` 调用或事件回调），媒体核心再把本地描述交回 UI 发送。

### 2.3 媒体核心 ↔ 对端 / 服务端：WebRTC

媒体核心内部对每路流维护一个 `PeerConnection`：

- 中继模式：对端是服务端 SFU。
- 直连模式：对端是另一个客户端，信令经服务端转发（服务端不看 SDP 内容，只做定向投递）。

## 3. 数据流

**文字**：UI → WS `text.send` → 服务端持久化 → 广播 `text.new` → 其他客户端 UI。

**语音（中继）**：麦克风 → 媒体核心（采集/3A/Opus）→ SRTP → 服务端 SFU → SRTP → 各成员媒体核心（抖动缓冲/解码）→ 声卡。

**语音（直连）**：麦克风 → 媒体核心 → SRTP → 对端媒体核心。N 人频道即 N-1 条连接、N-1 次编码。

**屏幕共享（直连）**：桌面采集 → 编码 → 每个观看者一条 SRTP。观看者 N 人 = N 次编码、N 路上行。

**屏幕共享（中继）**：桌面采集 → 编码一次 → SRTP → SFU → 扇出给观看者。

**回落**：直连的 `PeerConnection` 进入 failed，媒体核心发 `media.path_failed` 事件 → UI 调 `pu_*_set_mode(relay)` → 服务端建立中继会话 → UI 显示"已切换到中继"。

## 4. 路径状态机

每一路流（voice / screen）独立维护：

```
Idle ──join──▶ Connecting ──ice ok──▶ Direct ──fail──▶ Switching ──▶ Relay
                    │                                               │
                    └──────────── user forces relay ────────────────┘
```

- `Connecting` 超过 5 秒或 ICE failed → `Fallback`（仅当当前模式是 direct）。
- 手动切换永远允许，且会重置 `PeerConnection`。
- 状态机只存在于媒体核心；UI 只显示结果，不参与决策。

## 5. 可观测性（产品差异点）

媒体核心每 2 秒产出一次 `pu_stats`：码率、帧率、丢包、RTT、jitter、当前路径、当前编码次数。

UI 必须显示：

- 每路流当前是 `直连` 还是 `中继`；
- 共享时"正在为 N 个观看者分别编码"；
- 最近一次路径切换的时间与原因。

这些不是调试面板，是主界面的一部分——双路径设计如果说不清走哪条，就等于没有双路径。

## 6. 部署

服务端单二进制：

```
partyup serve --data ./data --http :8080 --udp :50000-50099
```

- 首次启动生成邀请码并打印到 stdout。
- 数据：`data/partyup.db`（SQLite）+ `data/files/`（若后续加文件）。
- 备份 = 停服拷贝 `data/`。
- 不强制 TLS：局域网可直接用 `http://<ip>:8080`；公网部署建议前置 Caddy/Nginx。

## 7. 风险与第一个 spike

| 风险 | 影响 | 先验证什么 |
| --- | --- | --- |
| Avalonia 帧呈现（D3D 纹理 → Skia） | 决定整个 UI 方案成立与否 | **Spike 1**：C++ 侧生成 1080p60 测试帧，经共享纹理在 Avalonia 窗口里以 60fps 显示，测量 CPU 与掉帧 |
| libwebrtc 构建与体积 | 交付周期 | Spike 2：Windows 上拿到可链接的 libwebrtc（预编译或自建），跑通 loopback |
| 直连多观看者 = N 次编码 | 屏幕共享直连体验 | Spike 3：一路 1080p60 源，同时编 4 路，测 CPU 与上行 |
| Pion SFU 转发性能 | 单机容量 | Spike 4：1 发 5 收，测 CPU 与延迟 |
| Avalonia Linux Wayland | 第二平台 | v1 不做，记录在案 |

Spike 1 不通过时，降级方案是"BGRA 缓冲上传"（多一次内存拷贝），再不行才重评 GUI 框架——不要因为一个未验证的假设把整份架构推倒。
