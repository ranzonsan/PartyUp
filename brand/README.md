# PartyUp 品牌资产（v0.2 · 2026-11-04 定稿）

v0.2 的标识**只有一个记号**：自绘的 `P`。没有横向文字标，没有第二套字体。

- 记号：竖干**断成两段**（上半 300–536 · 下半 600–800），碗是一枚 **ember 圆环**
  （外径 236 = 上段墨迹长，与竖干**留 40 的缝**——这是作者定的断口，不要缝合）；
  两段竖干的墨迹为 **300–536 / 572–800**，中间缝 **36**，均与笔画粗细无关。
- 颜色：墨底 `#0B0C0E` / 骨头白 `#F4F1EA` / ember `#FF5A2B`。
- 笔画 **80**（56→68→80 三连调；88 时环的空腔闭合成点，是上限）。加粗只让杠变宽、环壁变厚。
- 图标 = 圆角方砖（半径 200/1024）+ 记号居中，记号高度占画布 **72%**。

## 文件

| 路径 | 内容 |
| --- | --- |
| `brand/v2/letters/p.svg` / `.png` | **记号源文件**（1024 画布，透明底 PNG 见 `p.png` 铺墨底版） |
| `brand/v2/icon.svg` / `icon.png` | 1024 应用图标（圆角方砖 + 记号） |
| `brand/v2/icon-mark.svg` / `.png` | 透明底记号，用于叠加到任意底色 |
| `brand/v2/icon-sheet.png` | 16–256px 缩放验收图 |
| `brand/ico/partyup.ico` | 多尺寸 ICO（256/128/64/48/32/24/16） |
| `assets/icon.ico` | 与上面同一个文件，供 `vpk --icon` 与 csproj `<ApplicationIcon>` 使用 |
| `brand/tools/build_icon.py` | 重出全套图标；`STROKE` / `GAP` / `FILL` / `RADIUS` 都是顶部常量，记号几何由笔画自动推出 |
| `brand/v2/BRIEF.md` | 记号的设计规格与禁区（改图前读） |
| `brand/v2/_superseded/` | 已废弃的逐字母自绘（a/r/t/y）与当时的说明，仅存档 |

## 重出

```bash
PYTHONPATH=.tools python3 brand/tools/build_icon.py      # 全套图标 + ICO
PYTHONPATH=.tools python3 brand/tools/render_svg.py brand/v2/letters/p.svg /tmp/p.png 1024 '#0B0C0E'
```

`scripts/make-assets.ps1` 是**占位图**生成器，会覆盖 `assets/icon.ico`；已加 `-Force` 闸门，
不带 `-Force` 时直接报错退出。真图标在 `assets/icon.ico`，不要跑那个脚本。

## 没做的

- 横向文字标：做过 `P + Ubuntu 的 arty`，用户 2026-11-04 裁决**弃用**（见 `LICENSE-ASSETS.md` 第 4 节留档）。
- `Up` 声波底座：始终未做，v0.2 定为不需要。
- 应用内 favicon / 托盘图标：等客户端接入时再用 `build_icon.py` 的同一份几何出对应尺寸。

## 许可

见 `brand/LICENSE-ASSETS.md`。记号为本项目原创；不含任何第三方字体二进制。
