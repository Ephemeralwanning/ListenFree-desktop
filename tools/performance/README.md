# Windows 原生性能复测

这是隔离诊断入口，不是产品版本。沿用正常 Release 的应用/QML 对象，只替换入口以执行固定场景并观察原生对象。使用 Qt 6.11.2 私有头文件统计已有 layer；不会为了统计给每个 Item 创建 layer。工具链路径对应当前 Windows 开发机的 `F:/QT`。

## 构建与运行

先按项目正常方式配置 `build/portable` 并准备 `dist/ListenFree-Portable` 运行库。以下命令在仓库根目录执行，后续编译独立使用 `build/performance-verify`，避免其他开发任务写入相同对象文件：

```powershell
python tools/performance/configure_isolated.py
$env:PATH='F:\QT\Tools\mingw1310_64\bin;F:\QT\6.11.2\mingw_64\bin;'+$env:PATH
& 'F:\QT\Tools\CMake_64\bin\cmake.exe' --build build/performance-verify --target listenfree -j 6
python tools/performance/build_probe.py
python tools/performance/prepare_cases.py --track 'E:/Music/new/AIZO - King Gnu.flac'
python tools/performance/measure.py build/performance-optimization/cases/acceptance-1.json
```

`prepare_cases.py` 支持 `--profile`、`--video`、`--output`、`--results`。首次准备时只读备份 SQLite，后续轮次共享该固定副本；新一组对照使用新的 output 目录。歌词反事实截图用例的固定时间位置针对 AIZO，其他曲目需调整 `freezeLyric` 和 `expectedLyricLines`。模板不包含用户资料库、音乐、登录信息或网络缓存。

`build_probe.py --link-only` 仅适用于入口和探针头文件均未改变、只重建了 QML/应用对象的情况。诊断 EXE 位于 `build/performance-optimization/runtime`，正常产品 EXE 位于 `build/performance-verify`；不要将诊断 EXE 用作便携交付。

## 场景与口径

- `acceptance-1/2/3`：各自冷启动 30 秒、普通播放、普通歌词页、全屏莫奈、退出回落。默认 threaded 渲染循环，开始前确认 Windows 已解锁。
- `browsing-final`：歌曲播放、发现页三张卡片、拼窗六次往返平移、艺术家主图、返回及回落。
- `acceptance-stress`：样式、频谱/氛围灯、唱片队列、本地 MV 暂停/恢复及释放、六轮沉浸进出、75 秒回落。
- `navigation-final`：六页状态恢复及返回后 0/16/50/100/250ms 截图。使用 basic 循环，密集截图会改变内存和时序，只用于功能/画面检查。
- `effects-scenes`：同窗口强制原效果与优化效果，比较固定歌词画面。
- `background-final`：固定背景尺寸和透明 PNG，检查全局旧图释放与艺术家合成画面。

Python 依赖 `psutil`；像素检查需要 Pillow。采样每 250ms 记录整个进程树的 PWS（USS）、WS、Private Commit、CPU、句柄；每秒读取 Windows PDH GPU 专用/共享内存。summary 的稳态值取各阶段末 5 秒中位数。渲染线程直接记录 afterRendering 间隔；它表示提交节奏，并非 GPU 执行时长。

采样临时调用 `SetThreadExecutionState` 防息屏/休眠，退出解除，不改持久电源计划。它不能解锁 Windows。每行同时记录 `sessionLocked`、`ownForeground` 和前台进程 ID；测试期间重新激活本测试窗口：锁屏、遮挡、没有连续提交的结果不作为前台性能验收。basic 循环和强制截图结果不能代替产品默认 threaded 帧时间。

运行时使用独立资料库，关闭自动扫描、启动播放和动态封面，音频输出为 null；三轮必须保持相同输入。对真实设备输出、动态封面等完整用户配置的结论需另测。探针不强制压缩工作集，不修改用户资料库。

```powershell
python tools/check_navigation_resources.py build/performance-optimization/results/navigation-final/objects.json
python tools/check_navigation_pixels.py build/performance-optimization/results/navigation-final
python tools/check_background_resources.py build/performance-optimization/results/background-final/objects.json
```

历史证据和已撤回的候选见 `docs/PERFORMANCE_OPTIMIZATION_2026-09-08.md`；不能把撤回版本的内存降幅算作最终收益。

## 固定版本 A/B

`python tools/performance/build_counterfactual.py` 暂时关闭六处本任务的资源优化，使用相同的其他源代码、依赖与诊断入口构建 `matched-baseline-runtime`，然后恢复六个源文件的原始字节并重建正常 Release。只能在没有其他构建和基准运行时执行；冲突检测会保留并报告并发修改。随后执行 `python tools/performance/build_probe.py --link-only` 重新链接优化诊断版本。这个对照是当前版本的开关对照，不是历史 EXE。

将同一份生成配置分别设置 `runtime` 为两个目录，并使用不同 `report` 目录。`compare_runs.py` 要求两个配置的步骤、曲目、资料库、窗口和渲染方式一致，检查会话/前台/连续渲染后生成对照数据。所有原始 PWS、WS、Commit、GPU、CPU 和提交间隔保留在结果中。

`expectedLyricLines: 89` 在进入歌词页前核对原生歌词负载；空歌词直接以退出码 10 拒绝，不能将该轮低占用当作优化收益。测试从隔离的空队列打开曲目，并清空隔离资料库中的待写标签任务，避免执行用户未完成的元数据写入。

`hidden-lyrics-return` 使用暂停的固定 50000ms 位置，对比返回后 0/16/50/100/250/2500ms 完整画面；`tools/check_hidden_lyric_pixels.py` 要求最大通道差不超过 2/255。该密集读回只验证画面，不用于前台内存或帧时间。

最终证据使用 `final-acceptance-1/2/3`、`matched-acceptance-stress-baseline/optimized`、`foreground-browsing-baseline/verified`。浏览验收还检查回落阶段没有额外导航/切歌。运行期间保持测试窗口自动执行，避免向测试窗口输入额外操作；Windows 前台锁定可能阻止程序激活，需查看实际前台采样，而不是只相信 requestActivate/SetForegroundWindow 的调用。此次经用户授权临时调整的电源、屏保和前台锁定等待均已恢复。

## 音源插件与真实在线播放补测

此前的本地音频用例只复制 SQLite，没有复制音源脚本；因此虽计入 SourceHost 进程，但没有覆盖已加载插件的运行时。不能将此前数字称为完整在线播放占用。

```powershell
python tools/performance/build_probe.py
python tools/performance/prepare_online_cases.py
python tools/performance/run_online.py build/performance-optimization/online-cases/online-acceptance-1.json
python tools/performance/summarize_online.py docs/validation/performance-optimization-2026-09-08/online-acceptance-1
```

`sourcesDirectory` 将当前安装脚本复制到隔离资料目录，产品中的路径重定向正常执行；不修改用户插件、资料库、队列或设置。`startWithoutSource` 仅清空隔离配置中的音源记录，随后通过真实导入接口加载当前插件，以测量同进程的加载增量。隔离资料库与脚本文本由 `.gitignore` 排除，不纳入交付证据。

`online-acceptance-1/2/3` 使用当前活动音源、酷我 450444 的真实元数据和线上 FLAC，覆盖插件加载、普通播放、普通歌词、全屏莫奈、在线 Seek、停止及清空队列后 45 秒回落。`online-switch-stress` 使用三个真实搜索结果、九次切歌及双向 Seek，启用智能过渡与动态封面，停止清空后观察 60 秒。`online-source-smoke-verified` 逐一加载当前安装的四份音源并实际起播。

探针新增音源加载状态、解析成功/失败计数、曲目身份、音频格式、歌词负载及队列大小。`summarize_online.py` 要求音源已加载、在线身份正确且解码进度持续推进，分别输出主程序、SourceHost 和控制台宿主的 PWS / WS / Commit。只有返回音频 URL、不实际解码，或者中途变成本地歌曲，均不能通过。

`ignoreUserInput` 仅在诊断窗口过滤外部鼠标/键盘事件，避免额外操作改变场景，不改变产品输入或视觉代码。`run_online.py --manage-foreground` 只在用户已授权调整测试设置时使用；临时屏保和前台锁定设置在 `finally` 恢复并核验，原值及恢复记录落盘。已有采样器的临时防休眠租约随进程退出解除。
