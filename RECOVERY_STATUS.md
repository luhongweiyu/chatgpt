# hcbyj source recovery status

## Acceptance criteria

The final replacement is accepted only when all of the following are true:

1. All 416 legacy `dmsoft` methods keep the same public name, parameter list/order/types and return type.
2. Return values, output parameters, error values, state transitions and observable side effects match the original x86 DLL.
3. Edge cases and invalid-input behavior are matched, not merely the common success path.
4. Each implementation is first validated against the original x86 DLL in a Win32 differential test where practical.
5. The validated source is then built from the same codebase as a native AMD64 DLL.
6. The final product has no dependency on the old x86 `hcbyj.dll`, no 32-bit proxy process, and no runtime forwarding to OP/COM/tools.dll.
7. The final wrapper-generator count must be **0**. A generated forwarding body is an unresolved method, not a recovered method.

## Build state

- The public interface contains 416 unique legacy virtual methods.
- CI builds the recovered source as both Win32 and x64.
- CI checks PE machine type for both architectures.
- A Win32 parity probe calls the original `hcbyj.dll` directly by the known RVA wrappers, so the original binary/source does not have to be committed to GitHub.

## Recovery state terminology

- **candidate**: a native C++ implementation exists but has not yet passed differential behavior tests.
- **verified-x86**: candidate behavior has matched the original x86 DLL for the covered test matrix.
- **verified-x64**: the same verified source builds/runs as x64 and passes architecture-specific tests.
- **unresolved**: still represented by generated compatibility forwarding or not behavior-recovered.

Compilation alone does not change a method from candidate/unresolved to verified.

## Current work

The current candidate set includes core process-memory primitives and several deterministic Windows/system/file/INI/window helpers. These are now being moved through the x86 differential harness before being treated as recovered.

Custom subsystems such as Asm/AsmCall, DmGuard, Faq, Foobar, registration/licensing behavior, special bind/input modes and other legacy-specific behavior remain unresolved until their original behavior has been recovered and tested.
