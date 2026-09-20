# hcbyj64 recovery status

Goal: produce a real AMD64 replacement for the legacy x86 hcbyj DLL, preserving the dmsoft API surface while replacing x86 RVA trampolines with x64 implementations.

## Invariants

- 416 legacy virtual methods are tracked.
- Every declared method must have exactly one implementation.
- No implementation may jump to the legacy x86 DLL by fixed RVA.
- CI must build with the x64 toolchain and verify PE Machine == 0x8664.
- Documented Win32/Win64 behavior is implemented natively where possible.
- Image/OCR/input/binding functionality is being migrated to a pinned x64 C++ backend (WallBreaker2/op 0.4.8.3) and then behavior-tested.
- Legacy custom subsystems (Asm/DmGuard/Faq/Foobar/registration) are not marked complete until their original behavior is recovered and tested.

## Current native migration batch

The current working tree has native Win64 implementations for memory/address primitives plus a new batch covering files/directories, ordinary INI access, object environment storage, screen/system metrics, memory/CPU usage, font smoothing, power-save inhibition, basic string/color conversion and OS build detection.

This branch is a recovery work branch; main should only receive a merge when the compatibility test matrix is complete.
