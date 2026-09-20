from __future__ import annotations

from pathlib import Path
import argparse
import csv


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--static-map", required=True,
                    help="CSV from extract_legacy_map.py")
    ap.add_argument("--runtime-vtable", required=True,
                    help="CSV from legacy_vtable_dump.exe")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    static_rows = read_csv(Path(args.static_map))
    runtime_rows = read_csv(Path(args.runtime_vtable))

    if len(static_rows) != 416:
        raise RuntimeError(f"expected 416 static methods, got {len(static_rows)}")
    if len(runtime_rows) != 417:
        raise RuntimeError(f"expected 417 runtime vtable rows, got {len(runtime_rows)}")

    runtime_by_slot = {}
    for row in runtime_rows:
        slot = int(row["slot"])
        if slot in runtime_by_slot:
            raise RuntimeError(f"duplicate runtime slot {slot}")
        runtime_by_slot[slot] = row

    if set(runtime_by_slot) != set(range(417)):
        raise RuntimeError("runtime vtable slots must be exactly 0..416")

    merged = []
    for row in static_rows:
        slot = int(row["slot"])
        runtime = runtime_by_slot[slot]
        merged.append({
            **row,
            "runtime_entry_va": runtime["entry_va"],
            "runtime_entry_rva": runtime["entry_rva"],
            "runtime_protect": runtime["protect"],
            "runtime_first32": runtime["first32"],
        })

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=merged[0].keys())
        w.writeheader()
        w.writerows(merged)

    print(f"merged {len(merged)} methods with runtime vtable entries -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
