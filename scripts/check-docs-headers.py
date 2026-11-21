#!/usr/bin/env python3
"""检查 docs/ 的状态头格式。

规则见 docs/adr/0007-docs-layering.md：每份内容型文档在文件前 6 行内必须有一行

    > 最后核对：YYYY-MM-DD · 对应 <版本号|未发版>

不检查：索引页 docs/README.md、ADR（用自身 _Status_ 行）、adr/TEMPLATE.md。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATTERN = re.compile(r"^> 最后核对：\d{4}-\d{2}-\d{2} · 对应 \S")

targets = [p for p in sorted((ROOT / "docs").glob("*.md")) if p.name != "README.md"]
targets += sorted((ROOT / "docs" / "research").glob("*.md"))

missing = []
for path in targets:
    head = path.read_text(encoding="utf-8").splitlines()[:6]
    if not any(PATTERN.match(line) for line in head):
        missing.append(path.relative_to(ROOT))

if missing:
    print("以下文档缺少或写错了状态头（应在文件前 6 行内）：")
    for path in missing:
        print(f"  - {path}")
    print("\n格式：> 最后核对：YYYY-MM-DD · 对应 <版本号|未发版>")
    sys.exit(1)

print(f"状态头检查通过：{len(targets)} 个文件")
