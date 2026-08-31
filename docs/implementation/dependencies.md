# Backend Bootstrap 依赖清单

## 已采用

| 依赖 | 版本 | 用途 | 链接方式 | 包体/常驻内存影响 | 替代方案 |
|---|---|---|---|---|---|
| Qt | 6.11.2 | Core、Gui、QML、Quick、SQL、Network、Multimedia、Concurrent、Test、ShaderTools | 系统外部 Qt kit，通过 CMake `find_package` | 由实际部署模块决定；不引入 WebEngine/Chromium | Qt 6.8 LTS（发布分支候选） |
| SQLite | Qt SQL 内置 SQLite driver | 本地库、设置、缓存和迁移 | 通过 Qt SQL，不额外嵌入第三方 ORM | 小型原生库；连接按线程创建并及时关闭 | 原生 SQLite C API（不优先） |
| CMake/Ninja | 3.30.5 / 1.12.1 | 构建和测试编排 | 项目级 Presets 与开发 shell | 仅构建期，不进入运行时 | Visual Studio generator |
| aqtinstall | 3.3.0 | 用户级安装 Qt 6.11.2 kit | Python 用户包，安装期使用 | 不进入应用包或运行时 | Qt Maintenance Tool（当前企业账户认证失败） |
| TagLib | 2.3.1 | 本地音频标签与时长读取 | vcpkg manifest，固定 baseline `2687ff07e8ca1e7f56030e809c8c2c5227975a7b`，动态链接；Debug/Release 共用 `.vcpkg_installed` | 扫描时创建短生命周期 `FileRef`；不缓存 TagLib 对象或封面；包体与峰值内存待基准测试记录 | ffmpeg/libavformat（过重）、按格式自研解析器（维护成本高） |

## 选择纪律

- Qt 官方模块优先于自研基础设施；
- 新增运行时第三方库必须先补版本、许可证、维护状态、包体和内存影响；
- 大型依赖（Electron、CEF、Qt WebEngine、常驻 Node）不得进入主进程；
- 所有可替换依赖通过应用接口隔离，并至少有 Mock 或替身测试；
- 依赖升级必须重新跑 Debug/Release、CTest、Smoke 和内存基线。

## TagLib 许可证与资源约束

- TagLib 以 `LGPL-2.1-only OR MPL-1.1` 双许可证发布；分发时保留上游许可证与版权声明，并按 MPL-1.1 路径核对打包清单；
- 读取器只提取标题、艺术家、专辑和时长，不读取或缓存内嵌封面；
- 每次读取使用栈上 `TagLib::FileRef`，调用结束即释放文件句柄和解析对象；
- 大规模扫描必须保持有界批次，后续用 1,000/10,000 首曲库记录耗时、峰值工作集和句柄数。
