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
- **Evidence:** Generated PCM WAV tests cover duration, decode, physical output open/write, play/pause/seek/resume/stop, repeated natural end, errors and media-file handle release. A bounded loopback HTTP server proves offline streaming. Fake-port tests prove sequential queue advancement, QueueModel synchronization and lyric transitions. Repeated adapter use and construction/destruction tests now pass the Windows handle gate with the pinned Qt Multimedia fix recorded under BID-008.
- **Alternatives:** Expose Qt media types to QML; put queue/end policy in the adapter; advertise intended FFmpeg/cubeb features before implementation.
- **Review later:** Replace the adapter behind the same ports when FFmpeg + cubeb is authorized; preserve owner-thread delivery or add an explicit dispatcher at that boundary.

## BID-008 — Carry a bounded Qt 6.11.2 WASAPI lifecycle patch

- **Question:** How should M2 close Qt 6.11.2's unpaired Windows MMCSS registration without changing the fixed Qt version or leaking Qt internals through the playback ports?
- **Choice:** Build Qt Multimedia from the exact `v6.11.2` source commit and apply one repository-owned patch that reverts each MMCSS task with a same-thread scope guard. Deploy only the patched `Qt6Multimedia.dll` beside repository Debug/Release binaries; leave the global Qt kit untouched.
- **Reason:** Microsoft requires `AvRevertMmThreadCharacteristics()` on the registering thread. This is the smallest fix that preserves Qt's intended audio-thread scheduling, satisfies the API contract and keeps the workaround inside the private Qt adapter/build boundary.
- **Reproducibility:** `scripts/build-patched-qtmultimedia.ps1` pins both Qt Multimedia and its build-only Vulkan headers, verifies commits and patch state, and produces a deterministic repository-local runtime. `scripts/build.ps1` invokes it by default; `-SkipQtMultimediaPatch` exists only for diagnosis.
- **Evidence:** The unpatched minimal control grew by 19–21 handles and retained `\Device\MMCSS`; the patched control was `584 -> 584` with no MMCSS handle. With the same product-test executable, the global unpatched DLL failed both retained lifecycle gates (`+22`, `+8`), while the patched DLL stayed at `+2`, `+1`; Debug/Release full CTest both passed 4/4.
- **Alternatives:** Disable Qt's MMCSS registration; select another Qt media backend; wait for upstream; replace Qt output with FFmpeg + cubeb. Disabling real-time scheduling changes playback behavior, while the other options have a larger capability or delivery risk.
- **Review later:** At every Qt Multimedia upgrade, inspect upstream for an equivalent fix. Remove the local patch only after the upstream source and full lifecycle tests prove it redundant.

## BID-009 — Attach every SourceHost process to a Windows Job Object

- **Question:** How should SourceHost descendants be cleaned up when the host crashes, times out, is force-terminated, or the client process exits unexpectedly?
- **Choice:** `SourceHostClient` creates a per-launch Windows Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, assigns the started host process, and terminates/closes the job on stop timeout, crash, finish and destructor paths. The handle is owned by a custom-deleter `std::unique_ptr`; non-Windows builds use a no-op boundary.
- **Reason:** Closing the job handle is an OS-enforced process-tree boundary, including parent-crash cleanup, and avoids polling or blocking waits on the controller thread. Explicit `TerminateJobObject` covers graceful-stop failures and already-exited roots with surviving descendants.
- **Alternatives:** Track child PIDs (racy and incomplete); recursive toolhelp termination (misses races and requires more permissions); wait-based cleanup (blocks the owner thread).
- **Evidence:** The independent fault Host spawns a `cmd.exe` child. Debug/Release tests verify non-zero child PID and exit after both graceful and forced stop, plus repeated start/stop with no remaining QTimer children or SourceHost processes. The production Host contains no fault injection.
- **Review later:** Revalidate nested-job behavior on the supported Windows 10/11 release environments and retain the same public SourceHost interface.
