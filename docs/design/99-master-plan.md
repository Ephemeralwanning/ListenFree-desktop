# 99 — ListenFree Desktop 总纲

> 状态：**已批准 / v0.1**
> 本文件已经从占位草案收束为可执行总纲，用户已明确授权开始 Backend Bootstrap 实施。

## 1. 设计真源

本总纲汇总以下文档：

1. `00-project-charter.md`：目标、范围和不可妥协约束；
2. `01-decisions.md`：状态为“已确认”的技术与产品决策；
3. `02-questionnaire.md`：已确认问答和推迟到后续版本的问题；
4. `03-figma-and-rust-bridge.md`：QML 生成物、手写包装层与 C++ 后端边界；
5. `04-mvp-compatibility-and-release.md`：MVP、JS 契约、在线服务、格式、性能与发布；
6. `05-backend-bootstrap-architecture.md`：功能区、依赖方向、核心接口、进程线程边界和里程碑。

发生冲突时以已确认决策和项目章程为先。候选方案必须保持可替换，不得被包装成已验证能力。

## 2. 产品与首个可运行范围

ListenFree Desktop 是面向 Windows 11、兼容 Windows 10 的 Qt Quick/QML 桌面音乐应用，主后端使用 C++20 + Qt。

Backend Bootstrap 必须跑通：

- 启动、退出和保存新项目自身设置；
- 受控目录的本地音乐扫描和 SQLite 索引；
- 本地文件与普通 HTTP/HTTPS 音频；
- 队列、播放/暂停/停止、Seek、音量、静音、基础歌词和结构化错误；
- 独立 SourceHost 的加载、初始化、请求、取消、超时、崩溃恢复和退出契约；
- 在线服务内部模型、Mock Provider 和网易云可替换边界；
- 供 QML 并行开发使用的控制器、列表模型和 Mock 数据。

本阶段不要求最终 UI、生产网易云协议、真实 JS 运行时的完整兼容率或未经测试的 B/C 级播放器能力。

## 3. 平台与工具链

- 平台：Windows 10/11 x64，Windows 11 优先；
- 语言：C++20；
- UI：Qt Quick/QML；
- 构建：CMake + Ninja；
- 正式候选：Qt 6.11.2 + MSVC 2022 x64（发布前验证）；
- 当前可复现验证环境：Qt 6.11.2 MinGW x64、CMake 3.30.5、Ninja 1.12.1、GCC 13.1；
- 数据库：Qt SQL + SQLite；
- 初始媒体后端：Qt Multimedia；
- 发布：`windeployqt` + CPack/Inno Setup，GitHub Releases 仅发布 x64 Setup EXE。

M5 前必须固定并验证正式发布工具链。当前 MinGW 环境用于立即可执行的开发验证；MSVC 2022 x64 仍作为发布候选，不能未经验证替代。

## 4. 系统边界

主应用进程包含 QML 包装层、`qmlbridge`、应用服务、纯 C++ 领域模型、SQLite/扫描/媒体/在线适配器和 SourceHost 客户端。

`listenfree-sourcehost.exe` 是独立、按需启动、可监督的插件进程，只包含版本化 IPC、运行时适配器和 `globalThis.lx` 兼容层。它不得链接主数据库、播放器、QML、Electron、CEF 或 Qt WebEngine。

依赖方向固定为：

```text
QML -> qmlbridge -> application -> domain ports <- adapters
                                      ^
                                      |
                         SourceHost client <-> SourceHost process
```

功能区、核心接口、禁止依赖和 Mermaid 架构图以 `05-backend-bootstrap-architecture.md` 为准；具体文件与函数在实现阶段按最小可维护方案拆分。

## 5. 数据与用户文件

初始 schema 覆盖 `tracks`、`artists`、`albums`、`track_artists`、`playlists`、`playlist_entries`、`local_files`、`play_history`、`settings`、`provider_cache` 和 `schema_migrations`。

- 每线程独立 SQLite 连接，外键开启；
- 迁移按版本、校验和及事务执行；
- Repository 不泄漏 SQL 类型；
- 测试只使用临时数据库；
- 扫描只进入明确选择的根目录；
- 不复制、移动、重命名、改写或删除用户音乐；
- 不迁移旧应用数据库、设置、历史、Cookie、缓存和插件配置。

## 6. 扫描、播放、在线与 SourceHost

### 本地扫描

后台线程池执行可取消的增量扫描。路径、大小和修改时间是首阶段增量键；元数据读取通过 `IMetadataReader` 解耦；目录符号链接默认关闭。

### 播放

`IAudioPlayer`、`IPlaybackBackend`、`IAudioDeviceService` 和 `IEqualizerService` 构成可替换边界。Qt Multimedia 只报告实测能力；Gapless、Crossfade、ReplayGain、Equalizer 和 HighResolution 默认关闭。

### 在线服务

QML 只依赖内部 `Track`、`Playlist`、`Chart` 和 `Account` 模型。`MockOnlineProvider` 提供确定性数据；`NeteaseProvider` 在 bootstrap 只建立 transport、mapper 和错误边界，不猜测加密流程。

### JS 音源

M3 先证明版本化 IPC、生命周期、超时、取消、错误、日志、崩溃恢复和 Mock 插件契约。之后才评估 QuickJS 和必要 Node 回退，并用真实参考脚本证明兼容率。

## 7. QML 桥接与设置

`AppController`、`LibraryController`、`PlayerController`、`PlaylistController`、`OnlineController` 和 `SettingsController` 通过 `Q_PROPERTY`、signals 和 `Q_INVOKABLE` 暴露轻量状态。

`TrackListModel`、`PlaylistListModel`、`QueueModel` 使用增量模型信号。耗时操作立即返回请求 ID，完成与错误通过 queued signals 返回。

设置使用类型安全的稳定英文键，覆盖音量、静音、播放模式、设备、音乐库目录、扫描、歌词、Provider、SourceHost 和日志级别。显示文案不作为存储键。

## 8. 测试与性能门禁

必须通过：

- 领域类型和队列；
- SQLite 首次/重复迁移、CRUD、外键/索引和事务回滚；
- 扫描扩展名、增量、取消与符号链接范围；
- 播放状态机和生成 WAV 的 Qt Multimedia 基础闭环；
- SourceHost 编解码、握手、超时、取消、错误、崩溃恢复和退出；
- Mock Provider；
- QAbstractListModel 角色与增量信号；
- Bootstrap QML 启动、读取 Mock 数据并退出。

M5 实测 Debug/Release、测试总数、启动耗时、数据库首次创建耗时、Release 空闲和播放生成 WAV 时的进程树 Private Working Set/Commit Size/进程数。每个性能场景至少三次，报告样本、中位数与最大值，不允许估算。

## 9. 实施阶段

### M0 — Repository and Build Bootstrap

输出 CMake、Presets、警告、应用/测试骨架和 Bootstrap QML。门禁为干净 Debug configure/build 与 Smoke Test。

### M1 — Domain and Database

输出领域类型、端口、迁移、Repository 和扫描边界。门禁为领域、SQLite、Repository、事务与扫描测试。

### M2 — Playback Core

输出播放器接口、状态机、Qt Multimedia、设备和 capability。门禁为生成音频闭环与真实 capability。

### M3 — SourceHost Contract

输出独立进程、IPC、Mock runtime、监督、超时和取消。门禁为进程契约与故障恢复测试。

### M4 — Online and QML Facade

输出 Provider 接口/Mock、控制器、模型、设置与 Mock 模式。门禁为 Provider、模型和 QML Smoke Test。

### M5 — Verification and Documentation

输出 Debug/Release、全量 CTest、性能实测、依赖/架构/风险文档。门禁为验收项全部通过或存在可复现硬阻塞证据。

每个里程碑独立提交。失败的适配器可以回退到 Mock，不允许用假实现冒充完成。

## 10. Git、依赖与安全

- 只在隔离 `backend/bootstrap` worktree 实施；
- 保护现有修改，不强推、不修改默认分支、不创建 PR；
- 每个里程碑使用清晰英文提交并在认证可用时安全推送；
- 第三方依赖记录版本、用途、链接、包体/内存、许可证和替代项；
- 禁止提交凭据、Cookie、个人数据、缓存、构建产物和用户音乐；
- 旧项目只读，Pixso/Figma 生成目录不写业务逻辑。

## 11. 完成定义

1. 干净目录可配置；
2. Debug 和 Release 构建成功，或 Release 有可复现工具链硬阻塞；
3. 全量 CTest 通过；
4. SQLite 迁移、CRUD、回滚和重复迁移通过；
5. 最小应用启动退出，QML 读取歌曲、歌单、队列与播放状态；
6. SourceHost 完成启动、通信、超时、取消、恢复和退出；
7. 生成的本地测试音频可播放，能力报告真实；
8. 性能数据按规定实测；
9. 未修改旧项目和生成 UI；
10. 公共接口、决策、依赖、限制与风险均有文档；
11. `backend/bootstrap` 包含阶段提交并安全推送。

## 12. 不阻塞 Bootstrap 的后续问题

- 最终 UI 信息架构、视觉令牌和逐字歌词动效；
- 扫描、下载和峰值性能预算；
- Qt Multimedia 切换 FFmpeg + cubeb 的量化门槛；
- QuickJS/Node 生产组合；
- 网易云生产协议；
- HLS、WebDAV、LAN、完整代理、下载和 C 级音频；
- Windows 10 安装器、代码签名与干净机发布细节。

这些问题已有可替换边界，不改变 M0–M5 的文件和端口结构。

## 批准记录

- 批准人：用户（本次任务明确授权）
- 批准日期：2026-08-30
- 批准版本：v0.1
- 批准提交：本次实施提交
