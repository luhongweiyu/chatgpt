from pathlib import Path
import re

header = Path(__file__).resolve().parents[1] / "include" / "legacy_dm_x64.h"
source = Path(__file__).resolve().parents[1] / "src" / "legacy_dm_x64.cpp"
h = header.read_text(encoding="utf-8-sig", errors="ignore")
s = source.read_text(encoding="utf-8-sig", errors="ignore")
for inc in sorted(source.parent.glob("legacy_dm_x64_part*.inc")):
    s += "\n" + inc.read_text(encoding="utf-8-sig", errors="ignore")
declared = []
for line in h.splitlines():
    line = line.strip()
    if line.startswith("virtual ") and not line.startswith("virtual ~"):
        m = re.match(r"virtual\s+(.+?)([A-Za-z_]\w*)\((.*)\);$", line)
        if m:
            declared.append(m.group(2))
implemented = re.findall(r"\bdmsoft::([A-Za-z_]\w*)\s*\(", s)
missing = [name for name in declared if name not in implemented]
dupes = sorted({name for name in implemented if implemented.count(name) > 1 and name in declared})
print(f"declared={len(declared)} implemented={sum(name in implemented for name in declared)}")
print("missing:", missing)
print("duplicates:", dupes)
raise SystemExit(1 if missing or dupes else 0)
