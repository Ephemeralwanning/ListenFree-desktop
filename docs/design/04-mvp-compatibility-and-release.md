# 04 — MVP、兼容性、在线服务、性能与发布

## 首个可运行 MVP

首个里程碑不是完整复刻旧应用，而是证明新架构能形成可靠闭环：

1. QML 主窗口能够启动、退出并保存新项目自身的最小设置。
2. 扫描本地音乐并建立新 SQLite 索引，不复制或修改音频文件。
3. 旧版自定义 JS 音源可以导入、初始化、请求播放地址并取消请求。
4. 新在线服务可以搜索并返回歌曲，网易云可以登录、显示每日推荐和我的歌单。
5. 播放队列、播放/暂停、上下一首、Seek、音量、基本歌词和错误提示可用。
6. 进程退出、网络超时、无效脚本和播放失败不会损坏音乐库或卡死主 UI。

完成该闭环后，再按已确认的“B 为正式目标、必要时 A 短期落地、最终 C”路线扩展播放能力。

## 旧 JS 音源的真实边界

旧项目 `src/main/modules/userApi/renderer/preload.js` 表明，自定义音源 API 版本为 `2.0.0`，通过 `globalThis.lx`/`window.lx` 提供以下能力：

- 事件：`request`、`inited`、`updateAlert`；
- 动作：`musicUrl`、`lyric`、`pic`；
- HTTP 请求与取消，默认/最大超时 60 秒，支持代理；
- AES、RSA、随机数、MD5、Buffer、zlib；
- 脚本信息、运行环境和当前脚本文本；
- 来源/音质白名单以及返回值长度、URL 协议、歌词结构验证。

旧版使用隐藏的 Electron `BrowserWindow`、`contextBridge` 和 Chromium JavaScript 环境执行脚本。新项目要求外部行为兼容，不要求复刻这个内部实现。兼容宿主必须放在独立进程，具备：

- 单请求超时与显式取消；
- 输入输出大小限制和结构校验；
- 进程崩溃自动恢复与频率限制；
- 主应用退出时强制收尾；
- 代理、TLS、编码、Buffer 和 Web/Node JavaScript 语义的参考脚本测试集。

搜索、排行榜、歌单等功能来自旧项目内置 `src/renderer/utils/musicSdk/`，不是自定义音源 v2 的一部分。新版不移植这些内部实现，只保留它们作为产品行为参考。

## 新在线服务架构

### Qt 能做什么

排行榜、搜索和歌单是远端 API 返回的数据，不是 Qt 自带的音乐平台功能。Qt 原生能力负责基础设施：

- `QNetworkAccessManager`：异步 HTTP/HTTPS、代理、超时和取消；
- `QNetworkCookieJar`：登录 Cookie 会话；
- `QJsonDocument`：JSON 解析；
- `QAbstractListModel`：向 QML 暴露排行榜、歌单和歌曲列表；
- SQLite：本地缓存、收藏映射和离线索引。

### 稳定接口

主程序只依赖 C++ `IOnlineProvider` 抽象，至少定义：

- 登录、退出、会话恢复；
- 搜索与搜索建议；
- 推荐、排行榜、歌单列表、歌单详情；
- 歌曲详情、播放链接、歌词和封面；
- 取消、超时、错误分类、限流与缓存策略。

QML 不得直接拼 URL 或解析平台 JSON。平台返回结构必须先转换为项目内部的 `Track`、`Playlist`、`Chart`、`Account` 模型。

### 网易云实现候选

1. **首选方向：C++/Qt 原生 `NeteaseProvider`**。复用 Qt Network，并研究 Qcm 的 `ncrequest`、Qcm 网易云后端及 QCloudMusicApi 的协议、加密和模型实现；只提取经过测试的必要部分。
2. **正确性基准与回退：NeteaseCloudMusicApiEnhanced**。旧项目已使用其 Node 包，接口覆盖广且仍活跃；可按需启动独立进程，但不允许常驻成为默认路径，除非原生实现未达到可靠性门槛。
3. **窄服务候选：ncm-api-rs**。它可以作为独立本地服务测试，不通过 Qt Bridge 接入。仓库当前较新，维护历史和 Windows 稳定性必须验证；README 的内存数字不能直接当成本项目结论。

候选以同一组真实账号和只读接口测试：二维码登录、会话恢复、每日推荐、我的歌单、歌单详情、搜索、歌词、播放地址、代理、超时、风控错误。比较启动时间、稳态内存、请求延迟、成功率、更新成本和 Windows 打包，再选生产实现。

## 本地媒体格式矩阵

旧项目元数据扫描列表包含：

`mp3, flac, m4a, mp4, aac, ogg, oga, opus, wav, ape, wv, aiff, aif, tta, wma`

新项目将其作为候选兼容矩阵，而不是无条件承诺。每种格式必须分别验证扫描、解码、Seek、时长、元数据、封面、歌词、异常文件和适用时的 ReplayGain/高解析信息。

MP3、FLAC、M4A/AAC、OGG/Opus、WAV 是首批门禁；APE、WV、AIFF/AIF、TTA、WMA、MP4 音频容器紧随其后。

## 性能预算 v0

### 测试机

- Windows 11；
- Intel Core i9-13980；
- NVIDIA RTX 4060 Laptop GPU。

### 内存

- 目标：正常使用低于 400 MiB；
- 统计：应用整个进程树的 Private Working Set，包含主应用、SourceHost 和项目启动的辅助进程；
- 报告：同时记录 Commit Size、GPU 专用/共享内存和进程数量，避免只优化一个表面指标；
- 场景：冷启动稳定 30 秒、普通播放且一个音源宿主已加载、全屏逐字歌词；
- 每个场景至少测量 3 次并报告中位数与最大值。

400 MiB 目前是稳态正常使用门槛。峰值、批量扫描、缓存预热和多个 SourceHost 场景需继续问答后定义。

## 安装与发布候选

### 共同部署层

无论选择哪种安装器，都先使用 Qt 的 `windeployqt --qmldir` 收集 Qt DLL、插件和 QML 模块，再显式补充 FFmpeg、音源宿主及其他第三方运行库。

### 方案比较

| 方案 | 适用情况 | 本项目判断 |
|---|---|---|
| CPack + Inno Setup EXE | 单体 Windows 桌面应用、安装目录、快捷方式、卸载、文件关联 | 推荐。CMake 3.27+ 原生有 Inno Setup 生成器，配置与主构建统一。 |
| CPack + NSIS EXE | 与旧项目安装体验接近、脚本生态成熟 | 可行备选；旧项目当前就是 NSIS。 |
| Qt Installer Framework | 多组件、在线仓库、维护工具和组件更新 | 当前不做自动更新，功能偏重；以后需要组件化更新时再考虑。 |
| WiX/MSI | 企业集中部署、组策略和标准 MSI 工作流 | 首版成本过高，企业需求出现后再增加。 |
| MSIX | Windows 现代包管理或商店分发 | 通常涉及签名和容器规则，当前 GitHub Release 首发不优先。 |
| 便携 ZIP | 解压即用、测试、诊断和无安装使用 | 建议与安装 EXE 同时发布，但必须明确便携数据目录，避免污染安装版配置。 |

### 推荐产物

GitHub Release 暂建议提供：

- `ListenFree-vX.Y.Z-win-x64-Setup.exe`；
- `ListenFree-vX.Y.Z-win-x64-portable.zip`；
- `SHA256SUMS.txt`；
- 后续条件成熟时再加代码签名、自动更新和其他架构。

该安装方案仍为候选，需用户确认后才能升级为正式决策。
