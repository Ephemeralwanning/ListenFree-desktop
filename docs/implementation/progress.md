# Backend Bootstrap 完成度

> 统计日期：2026-08-31。百分比是用于排期的工程估计；是否达到门禁以测试和总纲完成定义为准。

## 总体判断

- 已批准的 Backend Bootstrap：约 **63%**；
- 面向最终可日用产品的整体工程：约 **30%–35%**；
- M2 功能闭环已形成，但 Windows 音频会话句柄门禁仍被 BLK-003 阻断；M0、M1、M3、M4、M5 仍为部分完成。

| 里程碑 | 估计 | 已验证能力 | 主要缺口 |
|---|---:|---|---|
| M0 构建骨架 | 90% | Qt 6.11.2、CMake Presets、Debug/Release、QML Smoke CTest | 缺干净机与发布工具链验证 |
| M1 领域/数据库/扫描 | 70% | SQLite CRUD/FK/回滚、TagLib、64 首有界批次、真实入库 | size+mtime 增量扫描、完整 repository、迁移 checksum、符号链接测试 |
| M2 播放核心 | 95% | 生成 PCM WAV 与 loopback HTTP 的真实加载/播放/暂停/Seek/停止/结束测试；Mute/音量；格式与结构化错误；动态设备/capability；队列推进与歌词投影；媒体文件句柄和进程退出门禁 | BLK-003：Qt 6.11.2 Windows 输出每次真实会话残留一个 MMCSS 设备句柄，尚未满足生命周期硬门禁 |
| M3 SourceHost | 68% | 独立进程、纯帧 Hello/HelloAck、1 MiB 帧限制、异步监督、统一请求终态、超时/取消/崩溃恢复、输入背压 | 真实插件运行时、进程树 Job Object、独立故障测试 Host |
| M4 在线/QML 门面 | 40% | Mock Provider、列表模型、App/Library/Player Controller | 设置/歌单/在线控制器闭环、网易云可替换适配器、QML 自动化 Smoke |
| M5 验证/文档 | 20% | 双配置构建、CTest、依赖/决策/基线文档 | 三次性能样本、播放/SourceHost 进程树、MSVC/安装器、完整风险收口 |

## 已形成真实闭环

- `LibraryController → 有界后台扫描 → TagLib → TrackRepository → SQLite`；
- `QProcess → SourceHost → 版本化帧 → 结果/超时/取消/崩溃终态`；
- `IAudioPlayer → PlaybackService → PlayerController/QueueModel`，由生成 WAV 覆盖真实解码、输出、队列结束推进与歌词位置；
- `QtAudioPlayer → Qt Multimedia` 仅存在于 media 实现层，应用接口覆盖可替换事件、设备、capability、格式和错误边界；
- 本机 Qt 6.11.2 枚举到真实音频输出，`QAudioSink` 打开与 PCM 写入门禁通过；本地 loopback HTTP 离线播放通过；
- M2 暂不标记完成：`listenfree_playback_tests` 中两个适配器断言和强化后的纯 Qt 最小对照均稳定复现 MMCSS 句柄残留，详见 BLK-003。

## 当前推进顺序

1. 保持 BLK-003 的 raw Qt 最小复现，等待 Qt 6.11.2 兼容修复或在批准后以 FFmpeg + cubeb 替换实现；
2. 收口 SourceHost 测试 Host 和 Windows 进程树；
3. 补增量扫描、迁移 checksum 与缺失 repository；
4. 完成设置、歌单、在线控制器及 QML CTest Smoke；
5. 执行 M5 性能、MSVC、部署和安装器验收。
