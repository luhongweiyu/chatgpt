# x86 reference parity harness

This directory is intentionally split between tracked test infrastructure and **untracked private reference files**.

Put the three original files here locally:

```text
reference/private/hcbyj.dll
reference/private/legacy_dm.h
reference/private/legacy_dm.cpp
```

They are ignored by git and are never required by normal CI.

When configuring a **Win32** build, CMake detects these files and builds `hcbyj_parity.exe`.
That executable links the recovered implementation and a macro-renamed copy of the legacy RVA wrapper, so both can be called in one x86 process.

The parity rule is strict: for each migrated method we compare return values, output parameters and observable side effects. A method is not considered recovered merely because it compiles.
