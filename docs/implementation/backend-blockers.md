# Backend implementation blockers

## BLK-001 — Master plan is not approved

- **Observed:** 2026-08-30 (Asia/Shanghai)
- **Affected milestone:** M0 through M5 formal implementation
- **Status:** Resolved by explicit user approval on 2026-08-30

### Blocking rule

The repository's governing documents prohibited formal module scaffolding and bulk implementation until `docs/design/99-master-plan.md` was explicitly marked **Approved**:

- `AGENTS.md` requires the master plan to be approved before bulk scaffolding or migration.
- `docs/design/README.md` reports the current phase as requirements/design, with the master plan unapproved, and permits only read-only research, questions, disposable technical validation, and design-document maintenance.
- `docs/design/99-master-plan.md` is still marked `Unapproved / placeholder draft` and states that it cannot authorize bulk implementation before explicit user approval.
- The unattended backend request says that confirmed design documents take precedence when instructions conflict, so it does not override this gate.

### Evidence and attempted resolutions

1. Read `AGENTS.md` and every mandatory design Markdown file in the prescribed order. All three governing documents above consistently retain the gate.
2. Ran `git fetch --all --prune` successfully and inspected `origin/main`; commit `32d999ec2b1ca0a80f380f5f5de02eb910877cba` contains the same unapproved placeholder master plan.
3. Inspected every local/remote branch, worktree, and the history of `docs/design/99-master-plan.md`. Only `main`/`origin/main` exist as source refs and no approved master-plan revision is present.

### Work completed without crossing the gate

- Preserved the clean `main` worktree and created the isolated `backend/bootstrap` worktree at `F:\player\lx-music-desktop-master\ListenFree-desktop.worktrees\backend-bootstrap` after verifying that the target did not exist.
- Confirmed remote fetch access.
- Audited the original environment: Qt 6.9.0 MinGW was present, while CMake, Ninja, MSVC 2022, and Qt MSVC were not discoverable in PATH or common Visual Studio locations. These setup gaps were resolved for bootstrap by installing Qt 6.10.3 MinGW and using the existing CMake/Ninja/GCC kit.
- Did not modify the legacy reference repository, Pixso/Figma assets, generated UI directories, or user music/data.

### Recovery steps (completed)

1. Review `docs/design/05-backend-bootstrap-architecture.md` and the consolidated `docs/design/99-master-plan.md` v0.1. (Done)
2. Obtain explicit user approval and update both status/approval records to **Approved** in the design source of truth. (Done)
3. Rebase or update `backend/bootstrap` from that approved commit without overwriting other work. (Done)
4. Resume at M0 with the verified x64 Qt/CMake/Ninja toolchain, then implement and verify M0–M5. Validate the preferred MSVC 2022 release kit before M5 packaging. (In progress)

### Progress toward recovery

- On 2026-08-30, `docs/design/05-backend-bootstrap-architecture.md` was prepared with functional areas, dependency direction, core interface families, process/thread boundaries, data flow, and milestone gates.
- `docs/design/99-master-plan.md` was expanded from a placeholder into reviewable v0.1 and explicitly approved by the user on 2026-08-30.
- Qt 6.10.3 MinGW x64, CMake, Ninja, and required Qt modules were configured and verified through project-local scripts. Design approval is now recorded; implementation may proceed.

### Integrity note

No placeholder backend, fabricated capability, unmeasured performance result, or unverified compatibility claim has been created to make the blocked milestones appear complete.

## BLK-002 — GitHub TLS handshake prevents the latest push

- **Observed:** 2026-08-30 (Asia/Shanghai)
- **Affected work:** Publishing commit `8fc8019` from `backend/bootstrap`
- **Status:** Resolved; later normal pushes advanced `origin/backend/bootstrap` through `76ee502` without rewriting history.

### Evidence

`git push` failed three consecutive times and `git ls-remote --heads origin backend/bootstrap` failed independently with the same Schannel error: `failed to receive handshake, SSL/TLS connection failed`.

### Recovery

Recovery completed after GitHub/network TLS connectivity returned. Continue using normal pushes; do not force-push or alter remote history.

## Current baseline note

The historical Qt 6.10.3 setup notes above are superseded by the user's confirmation on 2026-08-30. Qt 6.11.2 MinGW x64 is installed at `F:\qt\6.11.2\mingw_64` and is now the fixed development baseline; Qt version selection is no longer a project blocker.

## BLK-003 — Windows playback-session handles do not reach a steady state

- **Observed:** 2026-08-31 (Asia/Shanghai)
- **Affected milestone:** M2 lifecycle hard gate
- **Status:** Confirmed external Qt 6.11.2 / Windows audio-output blocker; M2 remains 95% and is not marked complete

### Reproduction

With Qt 6.11.2 MinGW x64 on this Windows 11 / Realtek output host:

```powershell
cmake --build --preset windows-debug --target listenfree_playback_tests
.\build\windows-debug\listenfree_playback_tests.exe repeatedOpenPlayStopHasBoundedLifetime repeatedConstructionAndPlaybackHasBoundedHandles rawQtMultimediaLifecycleHasBoundedHandles -o -,txt
```

The same `QtAudioPlayer` after 20 warm-up cycles gained 22 process handles over the next 20 open/play/stop/clear cycles (`615 -> 637` in the final stabilized test; earlier runs included `627 -> 649`). Twenty warm-up construction/play/destruction cycles followed by five measured cycles gained eight to nine handles (`656 -> 664`, `613 -> 622`). A 100-cycle warm-up followed by 50 more cycles grew `706 -> 752`, so this is not a bounded one-time plugin initialization pool.

The original raw-Qt control stopped as soon as `QMediaPlayer::PlayingState` appeared and was therefore too weak: it could stop before the Windows audio worker had started. Requiring `QMediaPlayer::position() > 0` makes the standalone `QMediaPlayer + QAudioOutput + QAudioBufferOutput + QMediaDevices` control reproduce the defect (`642 -> 651`) without any ListenFree adapter code.

Final full runs of `ctest --preset test-debug --output-on-failure` and `ctest --preset test-release --output-on-failure` each passed 3 of 4 CTest targets; only `listenfree_playback_tests` failed. Running its 14 non-lifecycle-gate functions separately produced 16/16 QtTest passes including suite setup/cleanup. The three lifecycle assertions remain red by design and are not skipped.

### Controls and attempted fixes

- Every generated WAV was removable after `clear()` and destruction; no media file handle remained open.
- Waiting 2, 10, 35 and 65 seconds while processing Qt deferred deletes did not return the count to baseline.
- `QMediaPlayer` is stopped, source-cleared and detached from buffer/audio output; Qt signals are blocked before destructor cleanup, so teardown publishes no external signal.
- Removing the audio-buffer tap, changing detach order, selecting a null device, sharing the device watcher, and explicitly stopping before clear did not remove the growth.
- Sysinternals Handle 5.0 type snapshots around five measured sessions showed `File 36 -> 41`, `Thread 9 -> 12 -> 9`, and total handles `588 -> 602 -> 594`. Detailed enumeration identifies all five persistent `File` handles as `\Device\MMCSS`; generated WAV paths are absent. This is one unreleased Windows Multimedia Class Scheduler device handle per session.
- The strengthened raw Qt reproducer proves the persistent MMCSS handles are in the Qt 6.11.2 FFmpeg/Windows audio-output path, not in `QtAudioPlayer`, queue code, callbacks, or the test server.
- Test exit leaves zero `listenfree`, `listenfree-sourcehost` or `sourcehost` processes.

### Recovery

Keep both adapter assertions and the strengthened raw Qt assertion enabled and failing. Recovery requires either (1) a Qt 6.11.2-compatible upstream fix that closes the MMCSS registration when the Windows audio worker exits, or (2) the planned non-Qt playback backend (FFmpeg + cubeb) behind the existing narrow ports. Re-run all three lifecycle tests and verify the stabilized handle delta is at most three before marking M2 complete. Do not raise the threshold, skip the tests, or advertise the blocked capability as fully closed.
