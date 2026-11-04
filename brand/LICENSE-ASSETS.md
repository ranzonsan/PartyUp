# 品牌资产许可 / Brand asset licensing

本文件只覆盖 `brand/` 目录下的图形资产（标识、文字标、图标、品牌板、生成的位图）。
仓库中**代码**的许可是 **GPL-3.0**，见仓库根目录 [LICENSE](../LICENSE)。

## 1. 标识与名称

- **版权**：Copyright © 2026 PartyUp contributors. 保留所有权利。
- `brand/src/`、`brand/png/`、`brand/ico/`、`brand/preview/` 下的文件**不在 GPL-3.0 的授权范围内**。
- **允许**：在任何媒介中为**指向官方 PartyUp 项目**而使用标识（文章、视频、教程、分发未修改的安装包、说明「兼容 PartyUp」）。
- **不允许**：修改标识后继续当作 PartyUp 标识使用；把标识用于暗示官方背书、赞助或关联；把标识用作自己产品的图标或商标。
- **派生（fork）**：GPL-3.0 允许你改造并分发代码，但**名称与标识不随代码授权**。公开发布的派生版本必须换名、换标识，使其不会被误认为官方版本。
- 这与 GPL-3.0 第 7(e) 条一致：该条允许在许可证之外保留商标与署名要求。

需要超出上述范围的授权（例如商业合作、周边商品）请先联系项目维护者。

## 2. 第三方组成部分

| 组件 | 用途 | 许可 | 是否随 PartyUp 分发 |
| --- | --- | --- | --- |
| **Ubuntu Font Family** (Dalton Maag / Canonical) | v0.2 文字标字形来源（`arty` 四个字母） | **Ubuntu Font Licence 1.0**（非 MIT，Debian 视为 non-free） | **否**。仅用 fontTools 提取轮廓，仓库不含字体二进制。详见第 4 节。 |
| **Lato** (Łukasz Dziedzic) | v0.1 文字标字形来源（已停用） | SIL Open Font License 1.1 | 否。仅历史版本使用。 |
| **cairosvg** | SVG → PNG 光栅化（构建期） | LGPL-3.0 | 否。仅在 `tools/` 里作为开发工具调用，不链接、不打包进客户端或服务端。 |
| **fontTools** | 从 Lato 字体提取字形轮廓（构建期） | MIT | 否。同上，仅构建期。 |
| **cairo / librsvg** | cairosvg 的运行时依赖（系统库） | LGPL-2.1+ / LGPL-3.0 | 否。系统库，不随项目分发。 |

`brand/tools/*.py` 这些构建脚本本身随仓库以 GPL-3.0 分发（它们属于代码）。

## 3. 原创性说明

- 标识（ember 圆角方块 + 自绘几何 P）由本项目从零绘制，未使用任何第三方图标集或图标字体。
- 文字标使用 Lato Black 排版并手工调整字距；Lato 为 OFL 授权，允许这种使用。
- 设计时刻意避开了同类产品已有记号的构图（控制器/脸、井号、三条重叠圆、字母内嵌块等）；若发现与既有商标存在混淆风险，以先权利人为准，需要替换时应通过 `brand/tools/build_assets.py` 的参数整体重出，而不是局部修补。


## 4. 文字标字体变更（2026-11-04）：Lato → Ubuntu（**该方案已弃用，留档备查**）

> **现状**：2026-11-04 用户裁决放弃横向文字标，v0.2 只保留自绘 `P` 作为图标。
> 因此**实际交付物中不含任何 Ubuntu 字形**，本节仅记录当时的许可评估，供日后重启文字标时复用。

v0.2 起，文字标除首字母 P（自绘）外，其余字形来自 **Ubuntu Font Family**（Dalton Maag 设计，Canonical 委托，2010–2022）。

**它不是 MIT。** 许可证为 **Ubuntu Font Licence 1.0（UFL-1.0）**，本机 `dpkg` 记录为：

- `/usr/share/doc/fonts-ubuntu/copyright`：`License: Ubuntu-Font-Licence-1.0`，`Copyright: 2010–2022 Canonical Ltd`；Debian 明示「**UBFL 1.0 不符合 Debian 自由软件指导原则（DFSG）**，故该包只存在于 non-free」。
- `/usr/share/doc/fonts-ubuntu/TRADEMARKS.txt`：`Ubuntu and Canonical are registered trademarks of Canonical Ltd. The licence accompanying these works does not grant any rights under trademark law and all such rights are reserved.`

UFL-1.0 与 MIT 的关键差别：

| | MIT | UFL-1.0 |
| --- | --- | --- |
| 修改后分发 | 随便 | 必须继续用 UFL，且**衍生字体不得使用 "Ubuntu" 之名** |
| 单独售卖字体 | 可以 | 不行（只能随更大的软件包一起） |
| 商标授权 | 不涉及 | **明确不授予**，Ubuntu/Canonical 为注册商标 |
| DFSG 自由性 | 是 | **否**（Debian 判为 non-free） |

### 本项目的用法与结论

- 只把字形**转成轮廓**（`brand/tools/compose_wordmark.py` 用 fontTools 提取 path），仓库与发行物中**不含任何 `.ttf`/`.otf`**。由字体排出的图形（logo、标语、截图）不是 UFL 意义上的 "Font Software"，不产生 UFL 的附随义务。
- 因此**不要**把 Ubuntu 字体文件提交进仓库，也不要在客户端里 bundle；需要重出文字标时，从系统字体重新提取轮廓即可（脚本已固化：cap 693/1000 → 500，基线 800）。
- ⊕ **遗留风险（已评估，用户知情）**：UFL 不授予商标权。以 Ubuntu 的字形作为 PartyUp 品牌标识的一部分，在极端情况下可能被解读为与 Canonical 存在关联。当前判定为低风险（PartyUp 是独立的自托管工具，不涉及 Ubuntu 发行版生态），若日后进入商业分发或需要注册商标，建议换用 OFL 授权的近似字体（如 Lato Black / Fira Sans / Source Sans 3）重出文字标——`compose_wordmark.py` 只依赖 `--font` 路径，换字体是一行参数。

## 5. 说明

以上为项目内的许可声明，不构成法律意见。若 PartyUp 进入商业分发或需要在中国/欧盟/美国注册商标，请让执业商标代理人复核第 1、3 节。
