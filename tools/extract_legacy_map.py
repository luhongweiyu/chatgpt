from __future__ import annotations

from pathlib import Path
import argparse
import csv
import re
import struct


def parse_methods(text: str) -> list[str]:
    return [m.group(1) for m in re.finditer(
        r"virtual\\s+(?:const\\s+char\\s*\\*|LONGLONG|long|float|double)"
        r"\\s*([A-Za-z_]\\w*)\\s*\\(",
        text,
    )]


def parse_wrapper_rvas(cpp: str, names: list[str]) -> dict[str, int]:
    out: dict[str, int] = {}
    for name in names:
        p = cpp.find(f"dmsoft::{name}(")
        if p < 0:
            raise RuntimeError(f"missing wrapper: {name}")
        m = re.search(r"g_dm_hmodule\\s*\\+\\s*(\\d+)", cpp[p:p + 1600])
        if not m:
            raise RuntimeError(f"missing wrapper RVA: {name}")
        out[name] = int(m.group(1))
    return out


def decode_slot_from_wrapper(blob: bytes, start: int, end: int) -> int:
    b = blob[start:end]

    for i in range(max(0, len(b) - 8)):
        if b[i:i + 2] == b"\\x8b\\x81" and b[i + 6:i + 8] == b"\\xff\\xd0":
            return struct.unpack_from("<I", b, i + 2)[0] // 4
        if b[i:i + 2] == b"\\x8b\\x91" and b[i + 6:i + 8] == b"\\xff\\xd2":
            return struct.unpack_from("<I", b, i + 2)[0] // 4

    for i in range(max(0, len(b) - 5)):
        if b[i:i + 2] == b"\\x8b\\x41" and b[i + 3:i + 5] == b"\\xff\\xd0":
            return b[i + 2] // 4
        if b[i:i + 2] == b"\\x8b\\x51" and b[i + 3:i + 5] == b"\\xff\\xd2":
            return b[i + 2] // 4

    raise RuntimeError(f"no vtable call at wrapper RVA 0x{start:X}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True)
    ap.add_argument("--header", required=True)
    ap.add_argument("--cpp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--image-base", default="0x10000000")
    ap.add_argument("--text-start-rva", default="0x1000")
    ap.add_argument("--text-size", default="0x166AA0")
    ap.add_argument("--vtable-rva", default="0x168C7C")
    ap.add_argument(
        "--scalar-deleting-destructor-rva",
        default="0xA6D80",
        help="slot 0 target used to recover the fixed vtable XOR key",
    )
    a = ap.parse_args()

    blob = Path(a.dll).read_bytes()
    header = Path(a.header).read_text(encoding="utf-8-sig", errors="ignore")
    cpp = Path(a.cpp).read_text(encoding="utf-8-sig", errors="ignore")

    image_base = int(a.image_base, 0)
    text_start = int(a.text_start_rva, 0)
    text_end = text_start + int(a.text_size, 0)
    vtable_rva = int(a.vtable_rva, 0)
    scalar_dtor_va = image_base + int(a.scalar_deleting_destructor_rva, 0)

    names = parse_methods(header)
    if len(names) != 416 or len(set(names)) != 416:
        raise RuntimeError(f"expected 416 unique methods, got {len(names)}")

    rvas = parse_wrapper_rvas(cpp, names)
    ordered = sorted((rva, name) for name, rva in rvas.items())

    slots: dict[str, int] = {}
    for i, (rva, name) in enumerate(ordered):
        end = ordered[i + 1][0] if i + 1 < len(ordered) else rva + 0x100
        slots[name] = decode_slot_from_wrapper(blob, rva, end)

    if sorted(slots.values()) != list(range(1, 417)):
        raise RuntimeError("public method slots are not exactly 1..416")

    raw_slot0 = struct.unpack_from("<I", blob, vtable_rva)[0]
    xor_key = raw_slot0 ^ scalar_dtor_va

    decoded_all: list[int] = []
    for slot in range(417):
        raw = struct.unpack_from("<I", blob, vtable_rva + slot * 4)[0]
        va = raw ^ xor_key
        decoded_all.append(va)
        rva = va - image_base
        if not (text_start <= rva < text_end):
            raise RuntimeError(
                f"decoded slot {slot} target 0x{va:08X} is outside .text"
            )

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow([
            "method",
            "wrapper_rva",
            "slot",
            "raw_vtable_entry",
            "xor_key",
            "implementation_va",
            "implementation_rva",
        ])
        for name in names:
            slot = slots[name]
            raw = struct.unpack_from("<I", blob, vtable_rva + slot * 4)[0]
            va = decoded_all[slot]
            w.writerow([
                name,
                f"0x{rvas[name]:08X}",
                slot,
                f"0x{raw:08X}",
                f"0x{xor_key:08X}",
                f"0x{va:08X}",
                f"0x{va - image_base:08X}",
            ])

    print(
        f"verified 416 public methods, slots 1..416; "
        f"vtable xor key=0x{xor_key:08X}; "
        f"decoded 417/417 targets inside .text -> {out}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
