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

## BID-005 — Scan and parse local metadata in bounded background batches

- **Question:** How should directory traversal and TagLib parsing avoid both an all-library path buffer and GUI-thread file I/O?
- **Choice:** Use one QtConcurrent worker to enumerate and parse serially, publish at most 64 tracks per batch, and let `QFutureWatcher` throttle production to two pending batches. Deliver application callbacks on the scanner object's thread. The application seam returns a `ScanId`, accepts idempotent cancellation by ID, and reports exactly one Completed/Cancelled/Failed terminal outcome.
- **Reason:** This removes the full path list, keeps TagLib use single-threaded, bounds large pending payloads to 128 tracks, and preserves QObject/UI thread affinity for consumers.
- **Resource constraints:** Delivered batch vectors are cleared immediately; cancellation cancels the future and the worker checks cancellation between files. A single filesystem or TagLib call remains cooperatively, not forcibly, cancellable.
- **Alternatives:** An explicit mutex/condition-variable channel; one worker per file; GUI-thread metadata parsing.
- **Review later:** Add measurable progress only when the UI needs it; retain scan generations, batch size and backpressure as implementation details.

## BID-006 — Make SourceHost supervision event-driven

- **Question:** How should SourceHost lifecycle and requests avoid blocking the QML/controller thread?
- **Choice:** Use QProcess signals plus member handshake, shutdown and restart timers. `start`, `request`, `cancel` and `stop` enqueue work and return without `waitFor*`; only the destructor retains a bounded emergency kill/wait fallback.
- **Reason:** Process startup, pipe writes and graceful shutdown are nondeterministic I/O. Keeping them inside the event-driven module preserves UI responsiveness and concentrates lifecycle cleanup in one implementation.
- **Terminal invariant:** Every accepted request ends exactly once as Succeeded, RemoteError, TimedOut, Cancelled, HostStopped, HostCrashed or WriteFailed. Late replies and repeated cancellation cannot complete it again.
- **Memory constraint:** Pending requests remain capped at 256; timers are deleted at their terminal transition; Host stdin delivery uses capacity-one blocking dispatch rather than accumulating queued frames.
- **Review later:** Move fault injection to a test Host and attach the Host process tree to a Windows Job Object before a Node runtime is introduced.

## BID-007 — Keep playback as a deep application module

- **Question:** How should Qt Multimedia deliver M2 playback without leaking framework media types or queue policy into QML?
- **Choice:** Keep `QMediaPlayer`, `QAudioOutput`, `QMediaDevices` and `QAudioBufferOutput` inside a Pimpl-backed `QtAudioPlayer`. The application boundary is split into `IAudioPlayer`, `IPlaybackBackend`, `IAudioDeviceService` and `IEqualizerService`; `PlaybackService` exclusively subscribes to player events and projects queue, end-of-media and lyric state to the controller. `PlayerController` accepts an owned `IAudioPlayer` for replacement/testing and reaches optional backend/device features only through their application ports.
- **State invariant:** An absent or cleared source is `Idle`. Loading, buffering, user stop, invalid media and natural end are reduced from an explicit Qt-independent observation; a stale `StoppedState` from the previous source cannot overwrite a new `Loading` transition.
- **Capability invariant:** Local file, HTTP stream, seek, volume and mute are reported only when the Qt backend is available. Device selection follows the live output inventory. Equalizer, gapless, crossfade, ReplayGain and high-resolution claims remain disabled until independently implemented and tested.
- **Callback/lifetime contract:** Playback and device callbacks are serialized on the player owner thread. Clearing a callback set is a synchronization point, and callbacks may re-enter player methods. Owners detach callbacks before destroying services or adapters. Qt children, media outputs and test sockets have deterministic RAII/parent-owned teardown.
- **Evidence:** Generated PCM WAV tests cover duration, decode, physical output open/write, play/pause/seek/resume/stop, repeated natural end, errors and media-file handle release. A bounded loopback HTTP server proves offline streaming. Fake-port tests prove sequential queue advancement, QueueModel synchronization and lyric transitions. Adapter and standalone raw-Qt tests keep the Windows MMCSS process-handle defect reproducible and intentionally red under BLK-003.
- **Alternatives:** Expose Qt media types to QML; put queue/end policy in the adapter; advertise intended FFmpeg/cubeb features before implementation.
- **Review later:** Replace the adapter behind the same ports when FFmpeg + cubeb is authorized; preserve owner-thread delivery or add an explicit dispatcher at that boundary.
