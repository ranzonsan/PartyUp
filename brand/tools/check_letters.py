#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按 BRIEF 抽查 brand/v2/letters/*.svg 的硬性参数（粗检，不做像素级验收）。"""
import glob, os, re, sys
D = "brand/v2/letters"
for f in sorted(glob.glob(os.path.join(D, "*.svg"))):
    s = open(f, encoding="utf-8").read()
    name = os.path.basename(f)
    vb = re.search(r'viewBox="([^"]+)"', s)
    circles = re.findall(r'<circle\b[^>]*>', s)
    rs = [float(m) for c in circles for m in re.findall(r'\br="([\d.]+)"', c)]
    has_bone = "#F4F1EA" in s.upper() or "#f4f1ea" in s
    has_ember = "#FF5A2B" in s.upper()
    nums = [float(x) for x in re.findall(r'-?\d+\.?\d*', re.sub(r'[a-zA-Z]', ' ', " ".join(re.findall(r'd="([^"]+)"', s))))]
    xs, ys = nums[0::2], nums[1::2]
    print(f"{name:8s} viewBox={vb.group(1) if vb else '??':16s} "
          f"circles={len(circles)} r={rs} bone={has_bone} ember={has_ember} "
          f"pathX=[{min(xs):.0f},{max(xs):.0f}] pathY=[{min(ys):.0f},{max(ys):.0f}]" if nums else f"{name:8s} no path")
