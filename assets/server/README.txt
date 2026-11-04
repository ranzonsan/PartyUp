PartyUp 服务端（Windows x64）
=============================

本包是 PartyUp 的服务端单二进制版：控制面（HTTP + WebSocket）、内建 STUN、
语音/屏幕共享的 SFU 中继全部在同一个 partyup.exe 里，不需要额外装数据库或媒体服务器。
数据存在本目录的 data\ 子目录（SQLite）。

文件
----
- partyup.exe   服务端主程序
- start.cmd     启动服务（双击运行）
- invite.cmd    另发一个邀请码（服务端运行中也可以跑，共用同一个 data 目录）
- data\         首次启动自动创建，存 SQLite 库；**备份这个目录就是备份服务器**
- LICENSE.txt   GPL-3.0

首次启动：邀请码怎么读
----------------------
1. 双击 start.cmd（或在本目录执行 `partyup.exe serve --data data --http 0.0.0.0:8080`）；
2. 控制台会打印机器可读的启动块，其中这两行最关键：

     BOOTSTRAP_TOKEN=...   管理员/引导令牌（只在本轮首次生成时出现）
     INVITE_CODE=...       邀请码，发给要加入的朋友

3. 看到最后一行 `READY` 表示已经监听成功。
4. 邀请码只在"本次启动新生成"时打印一次；之后要再发码，双击 invite.cmd，
   它会打印一行新的 `INVITE_CODE=...`。

默认监听
--------
- HTTP / WebSocket：0.0.0.0:8080（start.cmd 显式指定，局域网可达）
- STUN（UDP）：:3478（直连模式需要；relay 模式可关）
- 防火墙：第一次运行时 Windows 会询问是否允许访问网络，需要放行**专用网络**；
  若没弹窗，手动放行 TCP 8080 与 UDP 3478 入站。

常用参数（可直接加在 start.cmd 的 partyup.exe 那一行后面）
----------------------------------------------------------
  --http 0.0.0.0:8080        监听地址（默认 127.0.0.1:8080，只监听本机）
  --data data                数据目录
  --name "我的 PartyUp"       服务端展示名
  --voice-mode relay|direct  语音传输策略（默认 relay 中继；direct 为 P2P 直连）
  --screen-mode relay|direct 屏幕共享传输策略（默认 relay；direct 仅支持 1v1）
  --stun-listen :3478        内建 STUN 监听；传 off 关闭
  --stun-advertise IP:3478   多网卡/NAT 时指定对外通告地址
  --log-level info           日志级别 debug|info|warn|error

公网部署要点
------------
- 需要放行 TCP 8080（客户端连接）与 UDP 3478（STUN，若用 direct）；
- 服务端不内置 TURN：relay 模式下媒体走服务端转发，不需要 TURN；
- 有公网 IP 时建议给 --stun-advertise 填公网 IP:3478，否则直连只会拿到内网候选。

关于 Windows SmartScreen
------------------------
partyup.exe 没有代码签名。首次运行若被 SmartScreen 拦截：
点「更多信息」→「仍要运行」。这是未签名程序的正常提示。

版本
----
运行 `partyup.exe version` 查看版本（协议版本同时打印）。

许可证：GPL-3.0，见 LICENSE.txt。
