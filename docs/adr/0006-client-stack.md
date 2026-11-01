# 客户端用 Avalonia + C++ 媒体核心，不用 Qt

客户端 UI 用 Avalonia（C# / .NET 10，MIT），媒体能力放在一个 C++ 动态库（`partyup-media`，基于 libwebrtc）里，通过窄 C ABI 暴露给 C#。不使用 Qt。

_Status_: accepted · 2026-11-01

## 背景

项目许可证为 GPL-3.0，因此 MIT（Avalonia）与 LGPLv3（Qt）都可用，**许可证不再是选型因素**。剩下的是工程成本：UI 用什么写，媒体层怎么接。

## 实测与文档事实

- Qt 的 D3D 零拷贝路径是文档级 API：`QRhiWidget` + `QRhiTexture::createFrom()`，媒体层与 UI 同语言，无需跨边界。
- Avalonia 的等价路径要自己拼：`ICustomDrawOperation` 拿到 Skia 画布 → 从 DXGI share handle 导入 D3D 纹理 → 包成 EGLSurface → 绑到 GL 纹理 → 导入 Skia。官方在 `samples/GpuInterop` 有 D3D 示例，但社区讨论里明确说"第三步最难"。
- Avalonia 的 Linux 后端以 X11 为默认；原生 Wayland 后端到 12.1.0 才可选，官方文档自己标注"early stage"，且存在窗口 hide/show 无法重新映射的已知 bug。

## 决策理由

UI 部分（频道列表、文字、成员、设置）是典型业务界面，C#/XAML 的开发效率明显高于 C++/Qt，且不需要 moc、不需要处理 Qt Widgets 与 QML 的分裂。

媒体部分无论如何都是 C++（libwebrtc），所以 Qt 与 C++ 同语言的优势，实际上只省掉**一层边界**；而这一层边界可以做成窄接口、可单测的深模块——它是成本，也是隔离。

同时，"跨平台"在本项目里的难点不在窗口，而在**采集**（Windows Graphics Capture / WASAPI loopback、Linux PipeWire）与**渲染**，这两块都要为每个平台单独实现，换 UI 框架并不改变这一点。

## 考虑过的方案

- **Qt 6 Widgets + libwebrtc**：媒体同语言、零拷贝有现成 API；被否，因为 UI 开发效率与团队偏好，且 Qt 的 Widgets/QML 分裂让 UI 技术栈不统一。
- **纯 C#（SIPSorcery 等）**：被否。托管 WebRTC 编解码与 AEC 能力不足，`Microsoft.MixedReality.WebRTC` 已停更。
- **Rust + iced/egui**：被否。控件生态不足以支撑聊天界面，等于自造 UI 轮子。

## 后果与风险

1. **帧呈现是头号风险**：Avalonia 里 1080p60 的视频面必须靠自定义绘制 + D3D 纹理共享，路径未经验证。第一个 spike 就验它，验不过就退回"BGRA 缓冲上传"（多一次拷贝，仍可用），再不行才重新考虑 Qt。
2. 跨语言调试：崩溃在 C++ 侧时，C# 侧只能看到 `AccessViolation`。C ABI 必须做参数校验、错误码返回、禁止异常穿越边界。
3. Avalonia 的 Linux 支持弱于 Windows：v1 只承诺 Windows，Linux 放到第二个目标时再评估 Wayland 后端。
4. 打包体积：自包含 .NET + libwebrtc 会到 150–250 MB 量级；可接受，但要在安装包里如实说明。
