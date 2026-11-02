#!/usr/bin/env python3
"""Generate src/S411Symbols.h from the S-411 Annex C point symbols.

Usage:  python tools/gen_symbols.py <Annex_C_PortrayalCatalogue/Symbols>

The S-411 symbols use a small SVG subset (path with M/L/H/V/C/Z and the
relative l, circle, rect, group transforms scale/translate/rotate, one
stroke-dasharray). They are flattened here into polylines and filled
polygons in symbol millimetres (pivot at 0,0, y down), so the plugin can
draw them crisply at any size in both wxDC and OpenGL mode, recoloured per
palette, without an SVG renderer or extra files in the install package.
"""
import glob
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

SVG = "{http://www.w3.org/2000/svg}"


def mat_mul(a, b):
    return (a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],
            a[0] * b[2] + a[2] * b[3], a[1] * b[2] + a[3] * b[3],
            a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5])


def parse_transform(t):
    m = (1, 0, 0, 1, 0, 0)
    for name, args in re.findall(r"(\w+)\s*\(([^)]*)\)", t or ""):
        v = [float(x) for x in re.split(r"[\s,]+", args.strip()) if x]
        if name == "scale":
            sx = v[0]
            sy = v[1] if len(v) > 1 else sx
            n = (sx, 0, 0, sy, 0, 0)
        elif name == "translate":
            n = (1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0)
        elif name == "rotate":
            a = math.radians(v[0])
            c, s = math.cos(a), math.sin(a)
            n = (c, s, -s, c, 0, 0)
            if len(v) == 3:
                cx, cy = v[1], v[2]
                n = mat_mul(mat_mul((1, 0, 0, 1, cx, cy), n),
                            (1, 0, 0, 1, -cx, -cy))
        else:
            sys.exit("unsupported transform " + name)
        m = mat_mul(m, n)
    return m


def apply(m, p):
    return (m[0] * p[0] + m[2] * p[1] + m[4], m[1] * p[0] + m[3] * p[1] + m[5])


def path_polylines(d):
    toks = re.findall(r"[A-Za-z]|-?\d*\.?\d+(?:e-?\d+)?", d)
    i, cmd = 0, None
    cur, start = (0.0, 0.0), (0.0, 0.0)
    lines, line = [], []

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    while i < len(toks):
        if re.match(r"[A-Za-z]", toks[i]):
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                if line:
                    line.append(start)
                    lines.append(line)
                line = []
                cur = start
                continue
        if cmd in "Mm":
            x, y = num(), num()
            cur = (cur[0] + x, cur[1] + y) if cmd == "m" else (x, y)
            if len(line) > 1:
                lines.append(line)
            line, start = [cur], cur
            cmd = "l" if cmd == "m" else "L"  # implicit lineto after moveto
        elif cmd in "Ll":
            x, y = num(), num()
            cur = (cur[0] + x, cur[1] + y) if cmd == "l" else (x, y)
            line.append(cur)
        elif cmd in "Hh":
            x = num()
            cur = (cur[0] + x if cmd == "h" else x, cur[1])
            line.append(cur)
        elif cmd in "Vv":
            y = num()
            cur = (cur[0], cur[1] + y if cmd == "v" else y)
            line.append(cur)
        elif cmd in "Cc":
            pts = [(num(), num()) for _ in range(3)]
            if cmd == "c":
                pts = [(cur[0] + p[0], cur[1] + p[1]) for p in pts]
            p0 = cur
            for k in range(1, 9):
                t = k / 8.0
                u = 1 - t
                line.append((u ** 3 * p0[0] + 3 * u * u * t * pts[0][0] +
                             3 * u * t * t * pts[1][0] + t ** 3 * pts[2][0],
                             u ** 3 * p0[1] + 3 * u * u * t * pts[0][1] +
                             3 * u * t * t * pts[1][1] + t ** 3 * pts[2][1]))
            cur = pts[2]
        else:
            sys.exit("unsupported path command " + str(cmd))
    if len(line) > 1:
        lines.append(line)
    return lines


def dash(line, pattern):
    on, off = pattern
    out, seg, draw, left = [], [line[0]], True, on
    for a, b in zip(line, line[1:]):
        L = math.dist(a, b)
        pos = 0.0
        while L - pos > left:
            pos += left
            t = pos / L
            p = (a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]))
            if draw:
                seg.append(p)
                out.append(seg)
            seg = [p]
            draw = not draw
            left = on if draw else off
        left -= L - pos
        if draw:
            seg.append(b)
        else:
            seg = [b]
    if draw and len(seg) > 1:
        out.append(seg)
    return out


def walk(el, m, style, prims):
    style = dict(style)
    for k in ("stroke-width", "stroke-dasharray", "fill", "stroke"):
        if el.get(k) is not None:
            style[k] = el.get(k)
    cls = el.get("class", "")
    if "layout" in cls.split():
        return
    for c in cls.split():
        if c.startswith("f") and c[1:].startswith("ICE"):
            style["fillToken"] = c[1:]
        if c.startswith("s") and c[1:].startswith("ICE"):
            style["strokeToken"] = c[1:]
    m = mat_mul(m, parse_transform(el.get("transform")))
    tag = el.tag.replace(SVG, "")
    lines = []
    if tag == "path":
        lines = path_polylines(el.get("d"))
    elif tag == "circle":
        cx, cy, r = (float(el.get(k)) for k in ("cx", "cy", "r"))
        lines = [[(cx + r * math.cos(2 * math.pi * k / 24),
                   cy + r * math.sin(2 * math.pi * k / 24)) for k in range(25)]]
    elif tag == "rect":
        x, y, w, h = (float(el.get(k)) for k in ("x", "y", "width", "height"))
        lines = [[(x, y), (x + w, y), (x + w, y + h), (x, y + h), (x, y)]]
    for line in lines:
        filled = style.get("fill", "none") != "none" or "fillToken" in style
        scale = math.hypot(m[0], m[1])
        if filled:
            prims.append((1, 0.0, [apply(m, p) for p in line]))
            continue
        width = float(style.get("stroke-width", "1")) * scale
        parts = [line]
        if style.get("stroke-dasharray"):
            pat = [float(x) for x in re.split(r"[\s,]+", style["stroke-dasharray"])]
            parts = dash(line, pat[:2])
        for part in parts:
            prims.append((0, width, [apply(m, p) for p in part]))
    for c in el:
        walk(c, m, style, prims)
    return style


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    out = os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "src", "S411Symbols.h")
    symbols = []
    for f in sorted(glob.glob(os.path.join(src, "*.svg"))):
        name = os.path.splitext(os.path.basename(f))[0]
        root = ET.parse(f).getroot()
        prims = []
        token = "ICELIN"
        for el in root:
            if el.tag.replace(SVG, "") == "g":
                walk(el, (1, 0, 0, 1, 0, 0), {}, prims)
        if 'sICEBRG' in open(f, encoding="utf-8").read():
            token = "ICEBRG"
        symbols.append((name, token, prims))

    o = []
    o.append("/* S-411 Annex C point symbols as vector primitives - GENERATED by\n"
             " * tools/gen_symbols.py from Annex_C_PortrayalCatalogue/Symbols.\n"
             " * Do not edit. Units: symbol millimetres, pivot at (0,0), y down.\n"
             " * License of the symbol designs: CC BY 4.0 (S-411 Ed. 2.0.0). */\n")
    o.append("#pragma once\n\n#include <cstring>\n\n")
    o.append("struct S411SymPrim {\n  bool filled;     // polygon fill, else stroked polyline\n"
             "  float width;     // stroke width (mm)\n  int n;           // point count\n"
             "  const float *xy; // n x/y pairs (mm)\n};\n\n")
    o.append("struct S411Symbol {\n  const char *name;\n  const char *token;  // colour token\n"
             "  int count;\n  const S411SymPrim *prims;\n};\n\n")
    for name, token, prims in symbols:
        for k, (filled, width, pts) in enumerate(prims):
            flat = ", ".join("%.3ff, %.3ff" % (x, y) for x, y in pts)
            o.append("static const float s411_%s_%d[] = {%s};\n" % (name, k, flat))
        o.append("static const S411SymPrim s411_%s[] = {\n" % name)
        for k, (filled, width, pts) in enumerate(prims):
            o.append("  {%s, %.3ff, %d, s411_%s_%d},\n" % (
                "true" if filled else "false", width, len(pts), name, k))
        o.append("};\n\n")
    o.append("static const S411Symbol kS411Symbols[] = {\n")
    for name, token, prims in symbols:
        o.append('  {"%s", "%s", %d, s411_%s},\n' % (name, token, len(prims), name))
    o.append("};\n\n")
    o.append("inline const S411Symbol *S411FindSymbol(const char *name) {\n"
             "  for (const auto &s : kS411Symbols)\n"
             "    if (!std::strcmp(s.name, name)) return &s;\n"
             "  return nullptr;\n}\n")
    open(out, "w", encoding="utf-8", newline="\n").write("".join(o))
    print("%d symbols -> %s" % (len(symbols), out))


if __name__ == "__main__":
    main()
