# Backend implementation decisions

This log records reversible backend choices made without blocking design work. Confirmed architecture remains governed by `docs/design`.

## BID-001 — Use the installed Qt MinGW kit for design-stage verification

- **Question:** Which local toolchain should be configured before the backend architecture is approved?
- **Choice:** Use the already installed Qt 6.9.0 MinGW x64 kit with its bundled CMake 3.30.5 and Ninja 1.12.1 through project-local PowerShell scripts.
- **Reason:** The kit contains every module required by the bootstrap and can be verified immediately without a multi-gigabyte installation or system-wide PATH changes. No matching Qt MSVC kit is currently installed.
- **Alternatives:** Install Qt 6.8 LTS `msvc2022_64` and Visual Studio 2022 Build Tools now; use the installed MSVC 14.51 toolset from Visual Studio 2026 Insiders; permanently alter the user PATH.
- **Review later:** Yes. Before the first release build, validate and select the supported production kit, with Qt 6.8 LTS + MSVC 2022 x64 remaining the preferred candidate.

## BID-002 — Keep environment configuration project-local

- **Question:** Should tool paths be added permanently to the Windows user or system environment?
- **Choice:** Configure them only in `scripts/enter-dev-shell.ps1` and processes launched from that shell.
- **Reason:** This is deterministic, reversible, avoids conflicts with other Qt installations, and does not change machine-wide policy.
- **Alternatives:** User-level PATH changes; Qt Creator-only kit configuration; system-level PATH changes.
- **Review later:** No, unless CI requires a different entry point.
