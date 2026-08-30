# Backend implementation decisions

This log records reversible backend choices made without blocking design work. Confirmed architecture remains governed by `docs/design`.

## BID-001 — Use Qt 6.10.3 MinGW for bootstrap implementation

- **Question:** Which Qt kit should bootstrap implementation use now that implementation is authorized?
- **Choice:** Use Qt 6.10.3 MinGW x64 installed with user-level `aqtinstall` 3.3.0, plus CMake 3.30.5, Ninja 1.12.1 and GCC 13.1 through project-local PowerShell scripts.
- **Reason:** Qt 6.10.3 is the current stable 6.10 patch release and the machine already has the matching MinGW toolchain. The install includes Qt Quick/QML, SQL, Network, Multimedia, Test and ShaderTools without changing system PATH.
- **Alternatives:** Install a separate Qt 6.10.3 `msvc2022_64` kit and Visual Studio 2022 Build Tools; use the installed MSVC 14.51 toolset from Visual Studio 2026 Insiders; permanently alter the user PATH.
- **Review later:** Yes. Before release packaging, validate and select the supported MSVC 2022 x64 kit; the bootstrap interfaces must remain compiler/toolchain neutral.

## BID-002 — Keep environment configuration project-local

- **Question:** Should tool paths be added permanently to the Windows user or system environment?
- **Choice:** Configure them only in `scripts/enter-dev-shell.ps1` and processes launched from that shell.
- **Reason:** This is deterministic, reversible, avoids conflicts with other Qt installations, and does not change machine-wide policy.
- **Alternatives:** User-level PATH changes; Qt Creator-only kit configuration; system-level PATH changes.
- **Review later:** No, unless CI requires a different entry point.

## BID-003 — Defer Qt 6.11.2 until the installer feed is reproducible

- **Question:** Should the bootstrap immediately move beyond the requested Qt 6.10 line?
- **Choice:** Keep the verified development kit at Qt 6.10.3 for this milestone; revisit Qt 6.11.2 after a reproducible package feed or official installer is available.
- **Reason:** Qt 6.11.2 is a newer stable patch release, but the current `aqtinstall` metadata request fails while resolving `Updates.xml`. Switching kits mid-bootstrap would make the build less reproducible than the already-validated Qt 6.10.3 environment.
- **Review later:** Before release-candidate MSVC validation; the CMake requirement and module boundaries permit a minor-version upgrade without redesign.
