#!/usr/bin/env python3
"""Finds #include lines whose path only resolves case-insensitively (the
upstream port is built on Windows) and rewrites them to the real spelling.

    fix_include_case.py <repo> [--apply]
"""
import os
import re
import sys

repo = os.path.abspath(sys.argv[1])
apply = "--apply" in sys.argv
include_dirs = [os.path.join(repo, d) for d in (
    "platform/include", "decomp/include", "decomp/libs/JSystem/include",
    "decomp/libs/RVL_SDK/include", "decomp/libs/nw4r/include", "decomp/libs/RVLFaceLib/include")]

dir_cache = {}


def listing(d):
    if d not in dir_cache:
        try:
            dir_cache[d] = os.listdir(d)
        except OSError:
            dir_cache[d] = []
    return dir_cache[d]


def resolve_ci(base, rel):
    """The real relative path under base for rel, matched without case, or None."""
    cur, out = base, []
    for part in rel.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            cur = os.path.dirname(cur)
            out.append(part)
            continue
        names = listing(cur)
        if part in names:
            real = part
        else:
            low = [n for n in names if n.lower() == part.lower()]
            if len(low) != 1:
                return None
            real = low[0]
        out.append(real)
        cur = os.path.join(cur, real)
    return "/".join(out) if os.path.exists(cur) else None


inc_re = re.compile(r'^(\s*#\s*include\s*)([<"])([^>"]+)([>"])', re.M)
fixed = 0
for root, _, files in os.walk(repo):
    if "/.git" in root or "/build-" in root or "/third_party" in root:
        continue
    for f in files:
        if not f.endswith((".c", ".cpp", ".h", ".hpp", ".inc", ".inl")):
            continue
        path = os.path.join(root, f)
        try:
            text = open(path, encoding="utf-8", errors="surrogateescape").read()
        except OSError:
            continue
        changes = []
        for m in inc_re.finditer(text):
            rel = m.group(3)
            bases = ([root] if m.group(2) == '"' else []) + include_dirs
            if any(os.path.exists(os.path.join(b, rel)) for b in bases):
                continue
            for b in bases:
                real = resolve_ci(b, rel)
                if real and real != rel:
                    changes.append((m.start(3), m.end(3), rel, real))
                    break
        if changes:
            for s, e, rel, real in changes:
                print(f"{os.path.relpath(path, repo)}: {rel} -> {real}")
            fixed += len(changes)
            if apply:
                for s, e, rel, real in reversed(changes):
                    text = text[:s] + real + text[e:]
                open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text)
print(f"{fixed} include(s) {'fixed' if apply else 'to fix'}", file=sys.stderr)
