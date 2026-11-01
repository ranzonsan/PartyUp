# PartyUp

以频道为单位的自托管语音、文字与屏幕共享工具。本表定义产品语言里会反复出现的名词，**不含实现细节**。

## 语言

**Server（服务器）**
一次自托管部署，拥有自己的成员、频道与消息历史。一个 Server 一份数据，彼此不相通。
_Avoid_: 站点、实例、节点

**Member（成员）**
在某个 Server 上注册的身份。成员身份不跨 Server。
_Avoid_: 用户、账号、player

**Invitation（邀请码）**
加入某个 Server 的凭证。
_Avoid_: 邀请链接、注册码

**Channel（频道）**
Server 内的房间，同一时间承载文字区、语音区与共享区。
_Avoid_: 房间、大厅、群组

**Text Area（文字区）**
频道内持久保存的消息流。
_Avoid_: 聊天室、频道聊天

**Voice Area（语音区）**
频道内的实时语音场。进入语音区意味着"在这个频道说话"。
_Avoid_: 语音频道、语音房

**Share Area（共享区）**
频道内当前正在进行的屏幕共享的集合。
_Avoid_: 直播、投屏

**Session（会话）**
一个客户端连接某个 Server 的一次生命周期。
_Avoid_: 连接、登录态

**Stream（流）**
一路媒体，类型为 voice 或 screen。
_Avoid_: 轨道、track、feed

**Transport Path（传输路径）**
一路流实际经过的链路。
_Avoid_: 连接方式、模式

**Relay（中继）**
流经 Server 的 SFU 转发到达对端的路径。SFU 是服务端组件，Relay 是路径；文档里不要用 SFU 指路径。
_Avoid_: 转发、SFU（指路径时）

**Direct（直连）**
两端之间不经 Server 的路径，即口语说的 P2P。
_Avoid_: 点对点、P2P（作为正式名词时用 Direct）

**Fallback（回落）**
Direct 已建立但不可用时，自动切换到 Relay 的行为。**只用于失败，不用于择优。**
_Avoid_: 降级、切换、优化

**Sharer（共享者） / Viewer（观看者）**
发起屏幕共享的成员 / 观看该共享的成员。
_Avoid_: 主播、观众、播放端
