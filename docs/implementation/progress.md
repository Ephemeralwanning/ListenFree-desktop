# Backend Bootstrap 完成度

> 统计日期：2026-08-31。百分比是用于排期的工程估计；是否达到门禁以测试和总纲完成定义为准。

## 总体判断

- 已批准的 Backend Bootstrap：约 **67%**；
- 面向最终可日用产品的整体工程：约 **30%–35%**；
- M2 Playback Core 已完成全部当前门禁；M0、M1、M3、M4、M5 仍为部分完成。

| 里程碑 | 估计 | 已验证能力 | 主要缺口 |
|---|---:|---|---|
| M0 构建骨架 | 90% | Qt 6.11.2、CMake Presets、Debug/Release、QML Smoke CTest | 缺干净机与发布工具链验证 |
| M1 领域/数据库/扫描 | 70% | SQLite CRUD/FK/回滚、TagLib、64 首有界批次、真实入库 | size+mtime 增量扫描、完整 repository、迁移 checksum、符号链接测试 |
| M2 播放核心 | 100% | 生成 PCM WAV 与 loopback HTTP 的真实加载/播放/暂停/Seek/停止/结束测试；Mute/音量；格式与结构化错误；动态设备/capability；队列推进与歌词投影；媒体文件、MMCSS、进程和析构门禁；Debug/Release 各 4/4 CTest | 当前门禁无缺口；后续 FFmpeg + cubeb 可在相同窄接口后替换，但不是 M2 完成前置条件 |
| M3 SourceHost | 82% | 独立进程、纯帧 Hello/HelloAck、1 MiB 帧限制、异步监督、统一请求终态、超时/取消/崩溃恢复、输入背压、Windows Job Object 进程树托管、独立故障测试 Host 与 Debug/Release 故障门禁 | 真实插件运行时、生产插件契约兼容率 |
| M4 在线/QML 门面 | 40% | Mock Provider、列表模型、App/Library/Player Controller | 设置/歌单/在线控制器闭环、网易云可替换适配器、QML 自动化 Smoke |
| M5 验证/文档 | 20% | 双配置构建、CTest、依赖/决策/基线文档 | 三次性能样本、播放/SourceHost 进程树、MSVC/安装器、完整风险收口 |

## 已形成真实闭环

- `LibraryController → 有界后台扫描 → TagLib → TrackRepository → SQLite`；
- `QProcess → SourceHost → 版本化帧 → 结果/超时/取消/崩溃终态`；
- `IAudioPlayer → PlaybackService → PlayerController/QueueModel`，由生成 WAV 覆盖真实解码、输出、队列结束推进与歌词位置；
- `QtAudioPlayer → Qt Multimedia` 仅存在于 media 实现层，应用接口覆盖可替换事件、设备、capability、格式和错误边界；
- 本机 Qt 6.11.2 枚举到真实音频输出，`QAudioSink` 打开与 PCM 写入门禁通过；本地 loopback HTTP 离线播放通过；
- Qt 6.11.2 WASAPI 的未配对 MMCSS 注册已由固定源码补丁在同一 worker 线程回退；补丁诊断为 `584 → 584` 且无 `\Device\MMCSS` 残留，产品重复使用/构造门禁增长不超过两个，详见已解决的 BLK-003。

## 当前推进顺序

1. 接入真实插件运行时并以参考脚本证明 SourceHost 兼容率；
2. 补增量扫描、迁移 checksum 与缺失 repository；
3. 完成设置、歌单、在线控制器及 QML CTest Smoke；
4. 执行 M5 性能、MSVC、部署和安装器验收；
5. Qt Multimedia 升级时检查等价上游 MMCSS 修复，并在移除本地补丁前重跑完整生命周期门禁。

## 本轮 M3 收口证据

- `SourceHostClient` 在 Windows 为每次宿主启动创建 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` Job Object，并在 `QProcess::started` 后托管宿主；停止超时、崩溃、析构和进程结束路径都会终止并关闭 Job Object，避免子孙进程残留。
- 生产 `listenfree-sourcehost` 不再包含故障注入；故障场景由独立 `listenfree-sourcehost-fault-host` 测试 Host 提供。
- 新增故障测试覆盖启动失败、错误/延迟握手、请求、取消、超时、超大帧写失败、崩溃自动重启、优雅/强制停止、进程树清理及重复 start/stop 生命周期；Debug/Release 全量 CTest 均通过。
