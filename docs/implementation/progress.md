# Backend Bootstrap 完成度

> 统计日期：2026-08-31。百分比是用于排期的工程估计；是否达到门禁以测试和总纲完成定义为准。

## 总体判断

- 已批准的 Backend Bootstrap：约 **55%**；
- 面向最终可日用产品的整体工程：约 **25%–30%**；
- M0–M5 当前均为“部分完成”，尚无里程碑满足全部门禁。

| 里程碑 | 估计 | 已验证能力 | 主要缺口 |
|---|---:|---|---|
| M0 构建骨架 | 90% | Qt 6.11.2、CMake Presets、Debug/Release、QML Smoke CTest | 缺干净机与发布工具链验证 |
| M1 领域/数据库/扫描 | 70% | SQLite CRUD/FK/回滚、TagLib、64 首有界批次、真实入库 | size+mtime 增量扫描、完整 repository、迁移 checksum、符号链接测试 |
| M2 播放核心 | 40% | 状态机、Qt Multimedia 适配器、设备枚举、基础控制器 | 生成 WAV 真实播放闭环、Mute、队列联动、歌词、真实 capability 门禁 |
| M3 SourceHost | 68% | 独立进程、纯帧 Hello/HelloAck、1 MiB 帧限制、异步监督、统一请求终态、超时/取消/崩溃恢复、输入背压 | 真实插件运行时、进程树 Job Object、独立故障测试 Host |
| M4 在线/QML 门面 | 40% | Mock Provider、列表模型、App/Library/Player Controller | 设置/歌单/在线控制器闭环、网易云可替换适配器、QML 自动化 Smoke |
| M5 验证/文档 | 20% | 双配置构建、CTest、依赖/决策/基线文档 | 三次性能样本、播放/SourceHost 进程树、MSVC/安装器、完整风险收口 |

## 已形成真实闭环

- `LibraryController → 有界后台扫描 → TagLib → TrackRepository → SQLite`；
- `QProcess → SourceHost → 版本化帧 → 结果/超时/取消/崩溃终态`；
- Qt Multimedia 状态与设备适配器已接线，但真实音频输出门禁尚未闭合。

## 当前推进顺序

1. 收口 SourceHost 测试 Host 和 Windows 进程树；
2. 完成生成 WAV 的真实播放闭环并纠正 capability；
3. 补增量扫描、迁移 checksum 与缺失 repository；
4. 完成设置、歌单、在线控制器及 QML CTest Smoke；
5. 执行 M5 性能、MSVC、部署和安装器验收。
