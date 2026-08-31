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

## BLK-003 — Windows playback-session handles did not reach a steady state

- **Observed:** 2026-08-31 (Asia/Shanghai)
- **Affected milestone:** M2 lifecycle hard gate
- **Status:** Resolved locally with a pinned Qt 6.11.2 source patch; equivalent upstream fix remains open

### Root cause

Qt 6.11.2 `QWindowsAudioUtils::setMCSSForPeriodSize()` called `AvSetMmThreadCharacteristicsA()` for every WASAPI sink/source worker, discarded its returned task handle and never called `AvRevertMmThreadCharacteristics()`. Microsoft requires the revert call on the same thread that registered the task. Sysinternals Handle showed one persistent `\Device\MMCSS` file handle per real playback session; generated WAV handles and product processes were already released correctly.

The standalone `QMediaPlayer + QAudioOutput` reproducer gained 19–21 process handles across five measured sessions (`622 -> 641`, `619 -> 640`). Source and upstream evidence are recorded in `docs/research/qt-6.11-windows-mmcss-handle-leak.md`.

### Recovery implemented

- `patches/qt/6.11.2/0001-wasapi-revert-mmcss-registration.patch` changes the helper to return the task handle and installs a `qScopeGuard` in both sink and source worker lambdas. The guard calls `AvRevertMmThreadCharacteristics()` before the same worker thread exits.
- `scripts/build-patched-qtmultimedia.ps1` clones fixed Qt tag/commit `v6.11.2` / `6f162ccac1425edbd7b4d1582fabab5973b43d6c`, applies the patch, builds only Qt Multimedia and deploys the resulting DLL into repository build directories. It never overwrites `F:\qt\6.11.2\mingw_64`.
- The deployed Debug/Release DLL SHA-256 is `18FC9C8D214B6774BCE8067B4729AEF540753F6A8B2E20894DC66DE52B5E9495` for this build.
- Patched minimal diagnostic: `584 -> 584`, with zero `\Device\MMCSS` handles. Product gates: same-adapter loop `608 -> 610`; repeated construction/destruction `610 -> 611`.
- A controlled A/B using the same product-test executable and the global unpatched DLL made both retained product gates fail again: `635 -> 657` and `676 -> 684`. The local patch, not a skipped assertion or relaxed threshold, is what closes the gate.
- Final `ctest --preset test-debug --output-on-failure`: 4/4 passed; `listenfree_playback_tests` 161.58 s.
- Final `ctest --preset test-release --output-on-failure`: 4/4 passed; `listenfree_playback_tests` 160.95 s.
- Generated WAV files remained removable and test exit left zero `listenfree`, `listenfree-sourcehost`, `sourcehost` or playback-test processes.

### Upstream follow-up

The repository no longer has an M2 blocker, but the Qt kit itself remains affected. Before adopting a newer Qt 6.11.x build, check for an equivalent upstream revert, remove this patch only when it is redundant, rebuild the overlay and rerun both full configurations. Do not apply both fixes or deploy the unpatched global DLL over the repository runtime.
