# nlohmann/json（单头文件，vendored）

- 版本：**v3.11.3**
- 来源：<https://github.com/nlohmann/json/releases/tag/v3.11.3>
  （`single_include/nlohmann/json.hpp`，下载后原样入库，未做任何修改）
- 许可证：MIT（见文件头的 `SPDX-License-Identifier: MIT`）
- 用途：媒体核心解析 / 生成信令信封（`pu_signal_in` / `PU_EVENT_SIGNAL_OUT`）。
  C ABI 边界上仍然只有 `const char*` + 长度，JSON 库不跨越 DLL 边界。

## 为什么不用仓库自带的 jsoncpp

`third_party/libwebrtc/include/third_party/jsoncpp/` 只有头文件（14 个 `.h`），
没有 `src/lib_json/*.cpp`，也没有 `version.h` 之外的生成物；`webrtc.lib` 里也**没有**
任何 jsoncpp 实现符号（`nm webrtc.lib | grep "Json@@"` 为 0 条，只有 WinRT 的
`Windows.Data.Json` GUID）。所以直接用 jsoncpp 头文件会在链接期找不到
`Json::Value` / `Json::Reader` 的实现。

单头文件库是唯一「不加新源码、不改 `third_party/`」就能编过的选择。

升级方式：替换 `json.hpp` 全文 + 改本文件里的版本号，然后在
`scripts/build-media-dll.ps1` 下重跑构建与两个冒烟测试。
