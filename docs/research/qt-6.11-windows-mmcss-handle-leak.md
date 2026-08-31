# Qt 6.11 Windows MMCSS handle leak upstream research

> 调查快照：2026-08-31
> 目标版本：Qt Multimedia 6.11.2（tag `v6.11.2`，peeled commit `6f162ccac1425edbd7b4d1582fabab5973b43d6c`）
> 范围：Qt Bugreports、Qt Gerrit、`qt/qtmultimedia` 源码与提交、FFmpeg 官方源码、Microsoft MMCSS/AvRt 文档和样例。本文不把二手文章作为证据。

## 结论

Qt 6.11.2 的 Windows WASAPI 输出实现存在一条与实测现象完全吻合的资源缺口：`QWindowsAudioUtils::setMCSSForPeriodSize()` 调用 `AvSetMmThreadCharacteristicsA()` 得到 task `HANDLE`，随后只设置优先级，既不返回/保存该 handle，也从未调用 `AvRevertMmThreadCharacteristics()`。Microsoft 明确规定任务完成后必须调用 `AvRevertMmThreadCharacteristics()`，且必须在创建 handle 的同一线程调用。

Qt FFmpeg 播放后端在收到首个音频帧时创建并启动 `QAudioSink`；Windows 的 `QAudioSink` 落到 `QWASAPIAudioSinkStream`，每次启动新的输出工作线程都会执行上述 MMCSS 注册。线程停止路径会唤醒、`wait()` 并销毁 `QThread`，但工作线程 lambda 返回前没有 MMCSS 回退。因此，“真实 position 推进后，每个播放会话永久增加一个 `\Device\MMCSS` handle”是该缺失配对调用的直接、强证据结果。

截至调查快照，Qt 6.11 分支和 Qt Multimedia `dev` 仍保留同一缺口；没有找到对应公开 QTBUG、已合并修复 change 或已包含修复的 Qt 版本。Qt 已有的 WASAPI shutdown 修复只保证 drain shutdown 设置 stop request，Qt 6.11.2 已包含它；该修复没有增加 `AvRevertMmThreadCharacteristics()`，不能解决本问题。

ListenFree 已在固定的 Qt 6.11.2 源码提交上实施同线程 RAII 修复：sink/source worker 保存 `AvSetMmThreadCharacteristicsA()` 返回值，并在 worker lambda 退出时通过 scope guard 调用 `AvRevertMmThreadCharacteristics()`。补丁位于 `patches/qt/6.11.2/0001-wasapi-revert-mmcss-registration.patch`，构建脚本只把修复后的 `Qt6Multimedia.dll` 部署到仓库 Debug/Release 目录，不覆盖全局 Qt kit。

## 已证实事实

### 1. Qt FFmpeg 后端最终使用 Windows `QAudioSink`

Qt 6.11.2 的 FFmpeg `AudioRenderer` 在获得有效输出格式和首个音频帧后：

1. 在 `qffmpegaudiorenderer.cpp` 的 `AudioRenderer` 中以 `std::make_unique<QAudioSink>` 创建 sink；
2. 调用 `m_sink->start()` 启动它；
3. 以 `std::unique_ptr<QAudioSink> m_sink` 持有，说明解码由 FFmpeg 完成，但物理输出生命周期属于 Qt Audio。

直接源码：

- [`AudioRenderer` 创建并启动 `QAudioSink`，Qt 6.11.2](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/plugins/multimedia/ffmpeg/playbackengine/qffmpegaudiorenderer.cpp#L297-L325)
- [`AudioRenderer::m_sink` 所有权，Qt 6.11.2](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/plugins/multimedia/ffmpeg/playbackengine/qffmpegaudiorenderer_p.h#L114-L121)
- [`QWindowsAudioDevices::createAudioSink()` 返回 `QtWASAPI::QWindowsAudioSink`](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/multimedia/windows/qwindowsaudiodevices.cpp#L497-L508)

因此 FFmpeg 是解码/播放编排层，而产生 MMCSS 注册的物理输出线程属于 Qt 的 Windows WASAPI 层。

### 2. MMCSS handle 在 Qt 6.11.2 中被丢弃

`QWindowsAudioUtils::setMCSSForPeriodSize(reference_time)`：

- 周期小于 10 ms 时选择 `"Pro Audio"`，否则选择 `"Audio"`；
- 调用 `AvSetMmThreadCharacteristicsA()` 并把返回值放入局部 `HANDLE hTask`；
- 调用 `AvSetMmThreadPriority()`；
- 函数返回 `void`，没有 `AvRevertMmThreadCharacteristics()`，handle 也没有转移给任何 owner。

直接源码：[`qwindowsaudioutils.cpp` lines 485–503，Qt 6.11.2](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/multimedia/windows/qwindowsaudioutils.cpp#L485-L503)。其声明同样返回 `void`：[`qwindowsaudioutils_p.h`](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/multimedia/windows/qwindowsaudioutils_p.h#L80-L85)。

Qt 6.11.2 整个 `qtmultimedia` tree 中没有 `AvRevertMmThreadCharacteristics` 调用。可复核：

```powershell
git clone --depth 1 --branch v6.11.2 https://github.com/qt/qtmultimedia.git qtmultimedia-6.11.2
rg -n "AvSetMmThread|AvRevertMmThread|MMCSS" qtmultimedia-6.11.2
```

预期只找到 `qwindowsaudioutils.cpp` 的 `AvSetMmThreadCharacteristicsA` / `AvSetMmThreadPriority` 和构建文件中的 `avrt` 链接，不会找到 `AvRevertMmThreadCharacteristics`。

### 3. 注册和线程退出发生在同一工作线程，但退出前没有回退

`QWASAPIAudioSinkStream::startAudioClient()` 使用 `QThread::create()` 创建工作线程，lambda 的第一项操作就是 `setMCSSForPeriodSize(m_periodSize)`；之后进入 ringbuffer 或 callback 循环。所有正常 stop/error 返回最终都离开这个 lambda。直接源码：[`qwindowsaudiosink.cpp` 的工作线程创建和注册](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/multimedia/windows/qwindowsaudiosink.cpp#L211-L238)。

stop 路径会 request stop、停止/reset WASAPI client，并由 `joinWorkerThread()` 设置 event、等待线程结束、销毁 `QThread`；它没有 MMCSS handle，也没有能力在正确线程回退。直接源码：

- [`QWASAPIAudioSinkStream::stop()`](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/multimedia/windows/qwindowsaudiosink.cpp#L137-L163)
- [`QWASAPIAudioSinkStream::joinWorkerThread()`](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/multimedia/windows/qwindowsaudiosink.cpp#L334-L340)

输入侧 `QWASAPIAudioSourceStream::startAudioClient()` 也调用同一 helper，且同样没有 revert，因此该缺陷不只影响 playback：[`qwindowsaudiosource.cpp`](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/multimedia/windows/qwindowsaudiosource.cpp#L205-L229)。ListenFree 当前 M2 只触发输出侧。

### 4. Microsoft 要求显式、同线程配对回退

Microsoft 的 `AvSetMmThreadCharacteristicsA` 文档说明：

- 成功时返回 task handle；
- task 完成时调用 `AvRevertMmThreadCharacteristics()`。

来源：[AvSetMmThreadCharacteristicsA 官方文档](https://learn.microsoft.com/en-us/windows/win32/api/avrt/nf-avrt-avsetmmthreadcharacteristicsa)。

`AvRevertMmThreadCharacteristics()` 文档进一步规定：它接收 `AvSet...` 返回的 handle，并且必须由创建该 handle 的同一线程调用，否则失败。来源：[AvRevertMmThreadCharacteristics 官方文档](https://learn.microsoft.com/en-us/windows/win32/api/avrt/nf-avrt-avrevertmmthreadcharacteristics)。

Microsoft 的 WASAPI exclusive-mode 说明采用相同的 `< 10 ms => Pro Audio / >= 10 ms => Audio` 规则，并明确说停止时 `AvRevert...` 恢复原线程优先级。来源：[Exclusive-Mode Streams](https://learn.microsoft.com/en-us/windows/win32/coreaudio/exclusive-mode-streams)。官方 render sample 也在 render worker 的退出尾部调用 `AvRevertMmThreadCharacteristics(mmcssHandle)`，然后才 `CoUninitialize()` 和返回：[WASAPIRenderer.cpp](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/multimedia/audio/RenderExclusiveEventDriven/WASAPIRenderer.cpp#L2169-L2322)。较新的 Microsoft AEC sample 使用 scope-exit 保证同线程所有退出路径都回退：[AECCapture.cpp](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/AcousticEchoCancellation/cpp/AECCapture.cpp)。

### 5. 引入提交、受影响版本和现有 shutdown 修复

缺口由 Qt Gerrit change [631200, `Windows: audio utils - add some helpers / cleanup`](https://codereview.qt-project.org/c/qt/qtmultimedia/+/631200) 引入；主线提交为 [`ae1bf3ff6618dbddd344edab42bee8abd69dac23`](https://github.com/qt/qtmultimedia/commit/ae1bf3ff6618dbddd344edab42bee8abd69dac23)，Change-Id `I8bf91a290c6dcc42e6aa25e716e9804d19eb0605`。紧随其后的 sink rework 是 Gerrit [630775](https://codereview.qt-project.org/c/qt/qtmultimedia/+/630775)，主线提交 [`bca6400a8b88c6c0a30eec55b01617625ac371dd`](https://github.com/qt/qtmultimedia/commit/bca6400a8b88c6c0a30eec55b01617625ac371dd)，它把 helper 放进每个 WASAPI sink worker。

按上游 tag 逐一检查：

| 发布线 | 首个包含该 helper 的已发布 tag | 对照 |
|---|---|---|
| 6.8 LTS | `v6.8.4-lts-lgpl` | [`v6.8.3` 尚无 helper](https://github.com/qt/qtmultimedia/blob/v6.8.3/src/multimedia/windows/qwindowsaudioutils.cpp)，[`v6.8.4-lts-lgpl` 已包含](https://github.com/qt/qtmultimedia/blob/v6.8.4-lts-lgpl/src/multimedia/windows/qwindowsaudioutils.cpp) |
| 6.9 | `v6.9.1` | [`v6.9.0` 尚无 helper](https://github.com/qt/qtmultimedia/blob/v6.9.0/src/multimedia/windows/qwindowsaudioutils.cpp)，[`v6.9.1` 已包含](https://github.com/qt/qtmultimedia/blob/v6.9.1/src/multimedia/windows/qwindowsaudioutils.cpp) |
| 6.10 | `v6.10.0` | [`v6.10.0` 源码](https://github.com/qt/qtmultimedia/blob/v6.10.0/src/multimedia/windows/qwindowsaudioutils.cpp) |
| 6.11 | `v6.11.0` 起，含 `v6.11.2` | [`v6.11.2` 源码](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/multimedia/windows/qwindowsaudioutils.cpp#L485-L503) |

Qt 后来合并了 Gerrit [700105, `WASAPI: fix shutdown of wasapi sink streams`](https://codereview.qt-project.org/c/qt/qtmultimedia/+/700105)，并以 [701035](https://codereview.qt-project.org/c/qt/qtmultimedia/+/701035) cherry-pick 到 6.11，提交 `3e102eafa231123cd2550481909153a1317e3bfa` 已包含在 `v6.11.2`。该 change 只有一行实质变化：把 `requestStop()` 移到 shutdown policy 分支之前，使 drain 路径能够终止；它没有保存 task handle 或调用 `AvRevert...`。这解释了为何 ListenFree 能观察到 worker thread 数回落，而 `\Device\MMCSS` handles 仍持续累积。

### 6. 当前 upstream 没有公开修复

调查时固定的 upstream heads：

- Qt 6.11：`f74c8fa60d97a1de0a9ac54018b7ef07dcb43c13`
- Qt Multimedia dev：`06868e2acd1b3f352059424fa5f1e1ef2fd9e47b`

两者的 helper 仍调用 `AvSet...` 而没有 `AvRevert...`。可直接查看 [6.11 head 文件](https://github.com/qt/qtmultimedia/blob/f74c8fa60d97a1de0a9ac54018b7ef07dcb43c13/src/multimedia/windows/qwindowsaudioutils.cpp) 和 [dev head 文件](https://github.com/qt/qtmultimedia/blob/06868e2acd1b3f352059424fa5f1e1ef2fd9e47b/src/multimedia/windows/qwindowsaudioutils.cpp)。

Qt Bugreports 的以下官方查询均返回 0 issue：

- [`MMCSS`](https://bugreports.qt.io/rest/api/2/search/jql?jql=text%20~%20%22MMCSS%22&maxResults=100&fields=key,summary,status,fixVersions)
- [`AvSetMmThreadCharacteristics`](https://bugreports.qt.io/rest/api/2/search/jql?jql=text%20~%20%22AvSetMmThreadCharacteristics%22&maxResults=100&fields=key,summary,status,fixVersions)
- [`AvRevertMmThreadCharacteristics`](https://bugreports.qt.io/rest/api/2/search/jql?jql=text%20~%20%22AvRevertMmThreadCharacteristics%22&maxResults=100&fields=key,summary,status,fixVersions)
- [`WASAPI handle leak`](https://bugreports.qt.io/rest/api/2/search/jql?jql=text%20~%20%22WASAPI%20handle%20leak%22&maxResults=100&fields=key,summary,status,fixVersions)

`git log -S"AvRevertMmThreadCharacteristics" origin/dev -- .` 也没有提交。这里能证实的是“公开 tracker、已合并 git history 和调查时 open Gerrit subject 查询中未发现”；不能排除尚未公开或尚未以这些关键词提交的内部工作。

### 7. FFmpeg 7.1.5 本身不拥有这条 MMCSS 生命周期

Qt FFmpeg renderer 的源码已直接显示 Windows 输出由 `QAudioSink` 完成。进一步对 FFmpeg 官方 tag [`n7.1.5`](https://github.com/FFmpeg/FFmpeg/tree/n7.1.5)（commit `3a0867c2bfda4a4d4309ca1a8cbdc6175e67f587`）全 tree 搜索：

```powershell
rg -n -i "Av(Set|Revert)MmThread|MMCSS" FFmpeg-n7.1.5
rg --files FFmpeg-n7.1.5 | rg -i "wasapi"
```

两条命令均返回 exit code 1（无匹配）。FFmpeg 7.1.5 没有本问题所涉 AvRt 注册符号，也没有 WASAPI implementation 文件。结论仅限于这条 handle 的 owner：FFmpeg 仍负责解码和其自身线程，但 `\Device\MMCSS` 注册来自 Qt Windows Audio。

## 推断（与事实分开）

1. **高置信度根因推断：** 每个真实输出 session 增加一个 MMCSS handle，是因为每个 `QWASAPIAudioSinkStream` worker 各调用一次 `AvSet...`，返回 handle 被丢弃。Qt 调用次数、Microsoft 配对要求和 ListenFree 观察到的“一次 session 一个 handle”形成一一对应。
2. **position 条件的含义：** 仅进入 `PlayingState` 未必证明物理输出已开始；测试再要求 `position() > 0`，结合 `AudioRenderer` 在首个有效帧才创建/启动 `QAudioSink`，可以排除只初始化 player 而未走输出线程的假阳性。这是由 Qt 控制流推得，不是 Microsoft API 保证。
3. **线程正常结束不会替代 API 配对：** 6.11.2 已含 drain shutdown 修复且实测 thread 数可回落，但 handles 不回落；上游源码也没有任何其他 owner 能执行回退。因此不能把“worker 已退出”视为满足 AvRt contract。
4. **不能从主线程补救：** 即使 ListenFree 能枚举或记住 HANDLE，在 `QtAudioPlayer::stop()`/析构线程调用 `AvRevert...` 也违反 Microsoft 的同线程要求，不能作为有效应用层 workaround。

## 候选修复与本仓库验证顺序

### P0：在 Qt WASAPI worker 栈上增加同线程 RAII 配对（推荐）

修改上游 Qt 私有实现，使注册函数返回一个拥有 AvRt task handle 的 move-only guard，或直接在 sink/source worker lambda 中建立 scope guard。guard 的析构函数必须在同一个 worker lambda 返回前调用 `AvRevertMmThreadCharacteristics(handle)`；注册失败时不得调用 `AvSetMmThreadPriority(NULL, ...)`，回退失败应记录 `GetLastError()`。不要把 handle 交给 stream owner 后在 join caller 线程析构。

本仓库实现与验证顺序：

```powershell
$env:LISTENFREE_QT_ROOT='F:\qt\6.11.2\mingw_64'
$env:VCPKG_ROOT='F:\player\lx-music-desktop-master\_vendor\vcpkg'
$env:Path='F:\qt\6.11.2\mingw_64\bin;F:\qt\Tools\mingw1310_64\bin;F:\qt\Tools\Ninja;F:\qt\Tools\CMake_64\bin;' + $env:Path

.\scripts\build-patched-qtmultimedia.ps1 -Deploy
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset test-debug --output-on-failure
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset test-release --output-on-failure
```

诊断时在未补丁和已补丁的最小 `QMediaPlayer + QAudioOutput` 重现进程上分别运行 Sysinternals Handle：

```powershell
handle64.exe -accepteula -a -p <pid> | Select-String -SimpleMatch '\Device\MMCSS'
```

未补丁 DLL 的五次 measured session 从 `622 -> 641`（另一次 `619 -> 640`），并残留 `\Device\MMCSS` handles。已补丁 DLL 的最小重现为 `584 -> 584`，Sysinternals Handle 未发现 `\Device\MMCSS`。同一产品测试二进制的受控 DLL A/B 中，未补丁产品门禁为 `635 -> 657` 和 `676 -> 684`（2/2 失败），补丁版为 `608 -> 610` 和 `610 -> 611`。最终 Debug/Release 各 4/4 CTest 目标通过，WAV 仍真实推进 position、临时文件可删除，测试退出后无残留产品/测试进程。官方 change 合并并标注 target version 前，不宣称“upstream 已修”。

### P1：去掉 Qt 的手工 MMCSS 注册，作为根因 A/B 和临时 overlay

把 `setMCSSForPeriodSize()` 暂时改为空操作，重建相同 Qt overlay，重复 P0 的全部测试。预期 MMCSS handle 不再增长；若仍增长，说明另有未发现注册方。该方案牺牲 Qt 主动为 buffer worker 设置 `Audio/Pro Audio` 和 high/critical priority，必须增加 CPU 压力下的 underrun/glitch 测量，不能仅因 handle 门禁通过就作为长期发布修复。它排在 P0 之后，因为 Microsoft 提供了正确的配对 API，无需删除实时调度能力。

### P2：用 Qt `windows` media backend 做无需重编 Qt 的隔离/回退实验

Qt 官方实现允许以 `QT_MEDIA_BACKEND` 选择 backend；源码入口见 [`QPlatformMediaIntegration`](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/multimedia/platform/qplatformmediaintegration.cpp#L75-L99)，Windows plugin 名为 `windows`：[`qwindowsintegration.cpp`](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/plugins/multimedia/windows/qwindowsintegration.cpp#L27-L42)。

```powershell
$env:QT_MEDIA_BACKEND='windows'
ctest --preset test-debug --output-on-failure
Remove-Item Env:QT_MEDIA_BACKEND
```

判定：该变量只用于全量 A/B。如果 native player 绕过 Qt FFmpeg `AudioRenderer -> QAudioSink` 路径，MMCSS delta 应消失；但 `QAudioBufferOutput` 等 FFmpeg-only 行为和格式边界可能失败，因此必须跑全量 M2 tests，不能只看 handle。若 capability 或 HTTP/WAV 行为退化，该 backend 只能是诊断/回退，不是 M2 完成依据。

### P3：通过现有窄 ports 更换非 Qt 输出后端

若本地 Qt overlay 的维护/发布风险不可接受，则保留 `IAudioPlayer`/`IPlaybackBackend` seam，迁移到计划中的 FFmpeg + 独立音频输出库。该方案能绕开 Qt WASAPI 缺陷，但工作量、格式/设备/seek/HTTP/队列回归面最大，必须复用同一组 WAV、loopback HTTP、handle/thread/process 生命周期门禁。它不应先于只有数行且符合 Microsoft contract 的 P0 修复。

## 仍未知与外部动作

- 未知 Qt 内部是否已有未公开 QTBUG 或尚未上传 Gerrit 的修复；公开 upstream 在调查快照没有证据。
- 本仓库 P0 已构建并验证；仍未知 Qt 何时会接受等价上游修复。升级 Qt 6.11.x 后必须先检查补丁能否反向应用并重跑生命周期门禁，不能同时叠加等价 upstream fix。
- 未证明 `QT_MEDIA_BACKEND=windows` 能满足 ListenFree 的全部 M2 capability；它是隔离实验，不是已验证替代方案。
- Qt sink 与 source 共用缺陷。即使 ListenFree 当前只播放，向 Qt 上游报告时应覆盖 render 与 capture，并增加重复 start/stop 的 Windows handle-count test。
