# AGENTS.md

给在本仓库工作的 AI agent 的入口。人类读者请先看 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 读的顺序

1. [README.md](README.md) —— 产品是什么、怎么跑起来
2. [docs/BOUNDARIES.md](docs/BOUNDARIES.md) —— 四条边界，越界的工作直接停
3. [GLOSSARY.md](GLOSSARY.md) —— 命名，写文档和写代码都用这里的词
4. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) + [docs/PROTOCOL.md](docs/PROTOCOL.md) —— 按任务选读
5. `docs/adr/` 里与任务相关的 ADR —— 已经定过的事不要重新发明

## 仓库地图

| 路径 | 内容 | 语言 |
| --- | --- | --- |
| `server/` | 控制面 + Pion SFU + SQLite，单二进制 | Go |
| `client/` | 桌面客户端 | C# / .NET 10 + Avalonia |
| `src/media-core/` | 媒体核心，窄 C ABI 对外 | C++ / libwebrtc |
| `docs/` | 入库文档（面向使用者与贡献者） | Markdown |
| `.docs/` | 过程记录与实测证据，**gitignore，不入库** | Markdown |
| `scripts/` | 构建、打包、验收脚本 | PowerShell / Bash |
| `tests/` | 跨组件测试与 QA 脚本 | — |
| `.github/workflows/` | CI 与发布 | YAML |
| `brand/`、`assets/` | 品牌矢量源与打包输入 | — |

## 构建与验证

```bash
# 服务端
cd server && go build ./cmd/partyup && go test ./...

# 客户端
dotnet build client/PartyUp.Client.csproj -c Release

# 媒体核心（Windows，需要 MSVC + libwebrtc 预编译包）
pwsh -File scripts/build-media-dll.ps1
```

- C++ 部分只能在 Windows 上构建；已知坑见 [docs/DEV.md](docs/DEV.md)。
- 文档改动的验证：**所有相对链接必须可达**；新增文档要同时加进
  [docs/README.md](docs/README.md) 与 README 的文档表。

## 硬规则

1. **文档分层**：产品级、三个月后仍然成立的事实 → `docs/`；过程记录、实测数据、含内网 IP /
   机器名 / 临时 token 的内容 → `.docs/`。**绝不把 `.docs/` 的内容抄进 `docs/`。**
2. **命名以 GLOSSARY.md 为准**。代码标识符用英文，文档用中文；同一个概念不要造第二个词。
3. **不可逆决策先写 ADR**，`accepted` 之后再改代码。触发条件见 [GOVERNANCE.md](GOVERNANCE.md)。
4. **代码改动只用文件编辑工具逐处进行**。禁止用 heredoc、`sed`、Python 脚本、`echo >` 之类
   的方式批量改写代码——即使任务看起来更适合脚本。
5. **不手写成熟文件格式的解析器**（JSON、PNG、ZIP、Protobuf、ELF 等）。用第三方库，或避开解析。
6. **不为「省依赖」造轮子**。成熟能力优先用成熟实现。
7. **不要提交** `.docs/`、`.cache/`（含临时 bootstrap token）、`artifacts/`、`*.7z`/`*.zip`
   （libwebrtc 预编译包）。
8. **截图与录屏含成员隐私**：禁止 OCR、图像内容识别、上传到第三方服务。需要画面信息时，
   请维护者用文字描述。
9. **不改 LICENSE**。许可证变更必须走 ADR 并由项目 owner 决定。
10. **不新增云依赖、遥测、外部账号体系**（见 PRODUCT 非目标）。

## 完成的标准

- 代码改动：给出实际执行过的命令与输出，不要写「应该可以」。
- 文档改动：链接可达、与既有文档不矛盾、索引已更新。
- 验证不了：明确写「未验证 + 原因」，不要假装通过。

## 提交与推送

- 提交格式见 [CONTRIBUTING.md](CONTRIBUTING.md)「提交」。
- **不要自动 push**，除非明确授权；也不要在没有要求时打 tag 或发 release。
