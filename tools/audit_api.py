from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
header = root / "include" / "legacy_dm_x64.h"
source = root / "src" / "legacy_dm_x64.cpp"

h = header.read_text(encoding="utf-8-sig", errors="ignore")
for inc in sorted(header.parent.glob("legacy_dm_x64_decl*.inc")):
    h += "\n" + inc.read_text(encoding="utf-8-sig", errors="ignore")

src = source.read_text(encoding="utf-8-sig", errors="ignore")

decl_re = re.compile(
    r"^\s*virtual\s+(?:const\s+char\s*\*|LONGLONG|long|float|double)"
    r"\s*([A-Za-z_]\w*)\(.*\);\s*$",
    re.MULTILINE,
)
def_re = re.compile(r"\bdmsoft::([A-Za-z_]\w*)\s*\(")

declared = decl_re.findall(h)
native_defs = def_re.findall(src)
native_set = {name for name in native_defs if name in declared}
native_dupes = sorted({
    name for name in native_set if native_defs.count(name) > 1
})

generated_path = None
for candidate in (
    root / "build" / "generated" / "legacy_dm_generated.inc",
    root / "build" / "Release" / "generated" / "legacy_dm_generated.inc",
):
    if candidate.exists():
        generated_path = candidate
        break

generated_defs = []
if generated_path:
    generated_defs = def_re.findall(
        generated_path.read_text(encoding="utf-8-sig", errors="ignore")
    )
generated_set = {name for name in generated_defs if name in declared}

overlap = sorted(native_set & generated_set)
covered = native_set | generated_set
missing = [name for name in declared if name not in covered]

print(
    f"declared={len(declared)} unique={len(set(declared))} "
    f"native={len(native_set)} generated={len(generated_set)} "
    f"covered={len(covered)}"
)
print(f"unresolved_wrappers={len(generated_set)}")
print("missing:", missing)
print("native_duplicates:", native_dupes)
print("native_generated_overlap:", overlap)

bad = (
    len(declared) != 416
    or len(set(declared)) != 416
    or missing
    or native_dupes
    or overlap
)
raise SystemExit(1 if bad else 0)
