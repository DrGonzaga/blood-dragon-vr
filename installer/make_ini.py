"""Writes the shipped fc3vr.ini: the source ini with src/shipped-overrides.txt applied.
Usage: python3 make_ini.py <source ini> <overrides> <output ini>"""
import sys, re

src, ovr, out = sys.argv[1:4]
want = {}
drop = set()
for line in open(ovr, encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    if line.startswith("-"):  # -section.key = drop that key from the shipped file
        sec, key = line[1:].split(".", 1)
        drop.add((sec.lower(), key.lower()))
        continue
    k, v = line.split("=", 1)
    sec, key = k.split(".", 1)
    want.setdefault(sec.lower(), {})[key] = v

lines = open(src, encoding="utf-8", errors="surrogateescape").read().splitlines()
res, sec, done = [], None, set()

def flush_section(s):
    # keys this section still lacks go at its end
    for key, v in want.get(s, {}).items():
        if (s, key.lower()) not in done:
            res.append(f"{key}={v}")
            done.add((s, key.lower()))

for line in lines:
    m = re.match(r"\s*\[(.+)\]\s*$", line)
    if m:
        if sec is not None:
            flush_section(sec)
        sec = m.group(1).lower()
        res.append(line)
        continue
    m = re.match(r"\s*([^;=]+?)\s*=", line)
    if m and (sec, m.group(1).lower()) in drop:
        continue
    if sec in want and m:
        key = m.group(1)
        for k, v in want[sec].items():
            if k.lower() == key.lower():
                line = f"{key}={v}"
                done.add((sec, key.lower()))
    res.append(line)
if sec is not None:
    flush_section(sec)
for s in want:  # sections not in the file at all
    if not any(d[0] == s for d in done):
        res.append(f"[{s}]")
        flush_section(s)
open(out, "w", encoding="utf-8", errors="surrogateescape", newline="\r\n").write("\n".join(res) + "\n")
print("shipped ini:", out, "overrides:", ", ".join(f"{s}.{k}={v}" for s in want for k, v in want[s].items()))
