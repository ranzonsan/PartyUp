# PartyUp · Semi 修订预览

一次性、独立的原生桌面预览，不是正式客户端。真正使用 Semi.Avalonia 12.1.0.1 / Avalonia 12.1.4 / CommunityToolkit.Mvvm 8.4.2 / Material.Icons.Avalonia 3.0.2；不加载旧原型主题或 FluentTheme，不重写库的控件模板。

## 一条命令运行

Windows 调试桥 Worker 已启动时，从仓库根执行：

```bash
scripts/wdb.sh build-run --project docs/design/semi-preview/PartyUp.SemiPreview.csproj \
  --exclude third_party --exclude .cache --exclude .tools --exclude .design \
  --exclude artifacts --exclude .docs --exclude .research-tmp \
  --exclude src --exclude client --exclude server --exclude spikes
```

需要时按[调试桥说明](<../../../tools/windows-debug-bridge/README.md>)启动 Worker。停止用 `scripts/wdb.sh stop <任务ID>`，不是桥记录的中间构建PID。

## 本轮操作入口

1. 进入服务器先显示频道列表；单击浏览，**双击加入频道语音**。键盘选中条目后 Enter 同样加入。没有独立“加入此频道语音”按钮。
2. 正常左栏显示当前频道语音／其他在线／离线三组。图标依次为语音、麦克风、发声、扬声器、共享；悬停可读状态说明。颜色、图形与分组标题共同区分状态。
3. “切换频道”返回频道列表；“返回成员列表”回到成员视图。第一组暂按当前浏览频道的语音成员示范，原反馈措辞仍待最终确认。
4. 共享成员下的 **观看共享** 按钮手动开始观看；需先加入同一频道语音。观看区内嵌在常驻聊天上方，可切目标、停止、收起文字或主窗口全屏；Escape退出全屏。合成画面不代表接收了媒体。
5. 左下用户卡片点击打开菜单，“应用设置”打开右侧原生 SplitView 设置区，底部通话控制仍保留。
6. 消息输入框和发送按钮在同一行伸展，上下对齐、保持等高。
7. 顶部“登录页”进入本地登录；**在服务器上注册新账户** 是独立按钮，不是复选框。

## 分步注册（全部模拟）

- 输入带 HTTP/HTTPS 的服务器地址；无邀请码则下一步补填。
- 也可输入演示邀请链接 `https://example.invalid/?code=DEMO-2026`。由 System.Uri 和 Microsoft.AspNetCore.WebUtilities 10.0.10 解析，不手写 URI/query parser，不访问链接。
- `DEMO-2026` 是本地演示规则。模拟校验成功后才展示新用户名、显示名称、密码、重复密码。两次密码不一致不能创建。
- 创建仅完成内存故事板，**没有创建真实账户或消耗邀请码**。返回登录预填服务器和用户名，不保留密码。
- 当前真实服务端没有非消耗邀请码验证端点、显示名称字段或正式邀请链接约定；此 URL 格式不是已发布协议。

## 安全与后端边界

复用[纯内存状态](<../prototype/DemoState.cs>)，不构造生产媒体引擎；无网络、真实登录、麦克风/播放/采集、全局键、托盘/启动注册或数据持久化。演示工具默认折叠，状态视图不包含注册密码和邀请码内容。

所有未实现或未接入的服务端/内核能力持续登记[统一缺口台账](<../v1.1-backend-gaps.md>)，不能以截图或模拟成功关闭。完整需求见[六项改动清单](<change-requests.md>)。本预览不代表正式功能验收或全DPI矩阵通过，也未提交/推送或修改正式client/server/media-core。
