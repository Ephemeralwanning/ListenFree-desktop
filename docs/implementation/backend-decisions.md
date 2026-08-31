# Backend implementation decisions

This log records reversible backend choices made without blocking design work. Confirmed architecture remains governed by `docs/design`.

## BID-001 — Use Qt 6.11.2 MinGW for bootstrap implementation

- **Question:** Which Qt kit should bootstrap implementation use now that implementation is authorized?
- **Choice:** Use Qt 6.11.2 MinGW x64 installed under `F:\qt\6.11.2`, plus CMake 3.30.5, Ninja 1.12.1 and GCC 13.1 through project-local PowerShell scripts.
- **Reason:** The user confirmed Qt 6.11.2 as the fixed development baseline. The installed kit includes Qt Quick/QML, SQL, Network, Multimedia, Test and ShaderTools without changing system PATH.
- **Alternatives:** Validate a separate Qt 6.11.2 `msvc2022_64` kit for release packaging; permanently alter the user PATH.
- **Review later:** Yes. Before release packaging, validate and select the supported MSVC 2022 x64 kit; the bootstrap interfaces must remain compiler/toolchain neutral.

## BID-002 — Keep environment configuration project-local

- **Question:** Should tool paths be added permanently to the Windows user or system environment?
- **Choice:** Configure them only in `scripts/enter-dev-shell.ps1` and processes launched from that shell.
- **Reason:** This is deterministic, reversible, avoids conflicts with other Qt installations, and does not change machine-wide policy.
- **Alternatives:** User-level PATH changes; Qt Creator-only kit configuration; system-level PATH changes.
- **Review later:** No, unless CI requires a different entry point.

## BID-003 — Qt 6.11.2 is now the fixed project baseline

- **Question:** Should later work continue considering alternate Qt versions?
- **Choice:** No. Use Qt 6.11.2 for all subsequent development and verification unless a concrete Qt bug requires a temporary exception.
- **Reason:** The user confirmed the local Qt 6.11.2 installation and requested that version selection no longer block implementation.
- **Review later:** Only when a specific regression or security issue is reproduced.

## BID-004 — Use TagLib behind the metadata-reader port

- **Question:** How should the local-library adapter obtain common audio tags and duration without growing a custom parser stack?
- **Choice:** Use TagLib 2.3.1 through a locked vcpkg manifest and keep it private to `listenfree_library`; the application continues depending only on `IMetadataReader`.
- **Reason:** TagLib is mature, format-focused and substantially smaller in scope than embedding FFmpeg solely for metadata. A short-lived `FileRef` gives deterministic cleanup, while the basic filename reader remains the fallback for malformed or unsupported files.
- **Resource constraints:** Do not retain TagLib file/tag/property objects, do not load artwork in this reader, and process future scan results in bounded batches.
- **Alternatives:** FFmpeg/libavformat; custom per-format parsers; Qt-only filename metadata.
- **Review later:** After 1,000/10,000-track memory and handle baselines, or when a required format is unsupported.
