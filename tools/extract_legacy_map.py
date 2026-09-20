from __future__ import annotations

from pathlib import Path
import argparse
import csv
import re
import struct


DEFAULT_VTABLE_RVA = 0x168C7C


def methods(text: str) -> list[str]:
    return [
        m.group(1)
        for m in re.finditer(
            r"virtual\s+(?:const\s+char\s*\*|LONGLONG|long|float|double)"
            r"\s*([A-Za-z_]\w*)\s*\(",
            text,
        )
    ]


def wrapper_rvas(cpp: str, names: list[str]) -> dict[str, int]:
    out: dict[str, int] = {}
    for name in names:
        p = cpp.find(f"dmsoft::{name}(")
        if p < 0:
            raise RuntimeError(f"missing wrapper: {name}")
        m = re.search(r"g_dm_hmodule\s*\+\s*(\d+)", cpp[p:p + 1600])
        if not m:
            raise RuntimeError(f"missing wrapper RVA: {name}")
        out[name] = int(m.group(1))
    return out


def slot_from_wrapper(blob: bytes, start: int, end: int) -> int:
    b = blob[start:end]

    for i in range(max(0, len(b) - 8)):
        if b[i:i + 2] == b"\x8b\x81" and b[i + 6:i + 8] == b"\xff\xd0":
            return struct.unpack_from("<I", b, i + 2)[0] // 4
        if b[i:i + 2] == b"\x8b\x91" and b[i + 6:i + 8] == b"\xff\xd2":
            return struct.unpack_from("<I", b, i + 2)[0] // 4

    for i in range(max(0, len(b) - 5)):
        if b[i:i + 2] == b"\x8b\x41" and b[i + 3:i + 5] == b"\xff\xd0":
            return b[i + 2] // 4
        if b[i:i + 2] == b"\x8b\x51" and b[i + 3:i + 5] == b"\xff\xd2":
            return b[i + 2] // 4

    raise RuntimeError(f"no vtable call at wrapper RVA 0x{start:X}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True)
    ap.add_argument("--header", required=True)
    ap.add_argument("--cpp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--vtable-rva", default=hex(DEFAULT_VTABLE_RVA))
    args = ap.parse_args()

    blob = Path(args.dll).read_bytes()
    h = Path(args.header).read_text(encoding="utf-8-sig", errors="ignore")
    cpp = Path(args.cpp).read_text(encoding="utf-8-sig", errors="ignore")

    names = methods(h)
    if len(names) != 416 or len(set(names)) != 416:
        raise RuntimeError(f"expected 416 unique methods, got {len(names)}")

    rvas = wrapper_rvas(cpp, names)
    ordered = sorted((rva, name) for name, rva in rvas.items())

    slots: dict[str, int] = {}
    for i, (rva, name) in enumerate(ordered):
        end = ordered[i + 1][0] if i + 1 < len(ordered) else rva + 0x100
        slots[name] = slot_from_wrapper(blob, rva, end)

    if sorted(slots.values()) != list(range(1, 417)):
        raise RuntimeError("recovered slots are not exactly 1..416")

    vt = int(args.vtable_rva, 0)
    rows = []
    for name in names:
        s = slots[name]
        raw = struct.unpack_from("<I", blob, vt + s * 4)[0]
        rows.append({
            "method": name,
            "wrapper_rva": f"0x{rvas[name]:08X}",
            "slot": s,
            "vtable_byte_offset": f"0x{s * 4:04X}",
            "raw_file_vtable_entry": f"0x{raw:08X}",
        })

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=rows[0].keys())
        w.writeheader()
        w.writerows(rows)

    print(f"verified 416 wrappers and slots 1..416 -> {out}")
    print("NOTE: raw file vtable entries are protected/encoded; use runtime parity dump for true pointers.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
