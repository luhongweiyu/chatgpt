from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
header = root / "include" / "legacy_dm_x64.h"
source = root / "src" / "legacy_dm_x64.cpp"

h = header.read_text(encoding="utf-8-sig", errors="ignore")
for inc in sorted(header.parent.glob("legacy_dm_x64_decl*.inc")):
    h += "\n" + inc.read_text(encoding="utf-8-sig", errors="ignore")

s = source.read_text(encoding="utf-8-sig", errors="ignore")
for generated in (
    root / "build" / "generated" / "legacy_dm_generated.inc",
    root / "build" / "Release" / "generated" / "legacy_dm_generated.inc",
):
    if generated.exists():
        s += "\n" + generated.read_text(encoding="utf-8-sig", errors="ignore")
        break

decl_re = re.compile(
    r"^\s*virtual\s+(?:const\s+char\s*\*|LONGLONG|long|float|double)"
    r"\s*([A-Za-z_]\w*)\(.*\);\s*$",
    re.MULTILINE,
)
declared = decl_re.findall(h)
implemented = re.findall(r"\bdmsoft::([A-Za-z_]\w*)\s*\(", s)

missing = [name for name in declared if name not in implemented]
dupes = sorted({name for name in implemented if implemented.count(name) > 1 and name in declared})

print(f"declared={len(declared)} unique={len(set(declared))} implemented={sum(name in implemented for name in declared)}")
print("missing:", missing)
print("duplicates:", dupes)
raise SystemExit(1 if len(declared) != 416 or len(set(declared)) != 416 or missing or dupes else 0)
