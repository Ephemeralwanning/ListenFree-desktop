# 03 — Figma 与 Rust/Qt Bridge 工作流

## 名称澄清

以下三个名字容易混淆，但职责完全不同：

1. **Figma to Qt**：当前 Qt 官方的 Figma → QML 工作流。本项目的新设计采用它。
2. **Qt Bridge for Figma**：旧版设计导出插件，官方已停止积极开发；新项目不选用。
3. **Qt Bridges for Rust / qtbridge-rust**：Rust 后端与 Qt/QML 的绑定项目，与 Figma 无关；截至 2026-08-30 仍为 Public Beta，必须先做 PoC。

官方资料：

- Figma to Qt：https://doc.qt.io/figmatoqt/
- Qt Creator 导入流程：https://doc.qt.io/qtcreator/creator-how-to-add-designs-from-figmatoqt.html
- Qt Bridges：https://www.qt.io/development/qt-framework/qt-bridges
- Rust Bridge 仓库：https://github.com/qt/qtbridge-rust

## Figma → QML 边界

### 真源与目录约定

- Figma 保存视觉结构、组件、Variants、Variables/设计令牌和交互说明。
- Figma to Qt 的输出属于可再生成资产，计划放入 `ui/imported/figma/`。
- 手写 QML 组件、页面包装和适配器计划放入 `ui/components/` 与 `ui/pages/`。
- C++/Rust 模型、命令、数据库对象和播放器对象不得直接写进生成文件。
- 重新导出时允许覆盖 `ui/imported/figma/`，因此该目录不得存在唯一一份手写逻辑。

### 适合自动转换的内容

- 页面静态布局、Auto Layout、图标和图片资源；
- 字体、颜色、圆角、间距等 Variables/设计令牌；
- 能映射到 Qt Quick Controls 的基础交互组件；
- 组件状态和有限的视觉 Variant。

### 仍需工程实现的内容

- 数据模型、虚拟化列表、分页、缓存和错误状态；
- 播放器、数据库、音源、下载及系统集成；
- 高性能逐字歌词、频谱和复杂 Shader 动画；
- 键盘、无障碍、焦点、窗口缩放和多 DPI 验证；
- 对生成 QML 的绑定数量、过度绘制、纹理尺寸和内存进行性能审查。

Figma to Qt 可以帮助生成项目内容和模块级 CMake 文件，但不能替代完整的顶层构建、应用架构和行为实现。它是高价值的设计交接工具，不是“一键生成完整播放器”。

## Rust → Qt Bridges PoC

### 采用原因

Qt Bridges for Rust 的方向与“QML 前端 + Rust 后端”匹配，可把 Rust 类型、属性、方法、信号和模型暴露给 QML，并支持跨线程调用。若 PoC 通过，可减少手写 C ABI 或 C++ 胶水层。

### 当前风险

- 官方状态仍是 Public Beta，API、构建和部署方式可能变化。
- 当前官方要求 Qt 6.10+、Rust 1.88+、C++ 工具链以及 PATH 中可用的 `qmake`。
- Windows 支持以 x64 为主，必须在本项目实际 MSVC/Qt 环境中验证。
- Beta 依赖不应侵入播放器、数据库或音源核心；桥接层必须可替换。

### 最小验证范围

PoC 必须独立、可删除，且至少验证：

1. Windows 11 上从干净环境构建、启动和打包。
2. Rust QObject/单例向 QML 暴露属性、方法、信号和错误。
3. 1 万项列表模型的加载、筛选、增量更新和滚动帧时间。
4. Rust 后台线程向 QML 安全推送进度、歌词与播放器状态。
5. 异步取消、应用退出、异常与 panic 边界。
6. Debug/Release 调试体验、二进制体积、空闲内存和启动时间。

### 通过与回退

- 通过：构建和部署可重复，模型与线程语义清晰，没有阻断性崩溃或显著性能退化，才可将 Rust-first 提交为正式 ADR。
- 不通过：QML 和 Figma 工作流保持不变，桥接层改为薄 C++/Qt Facade；Rust 核心通过窄 C ABI 或独立进程接入。若这仍显著增加复杂度，则直接采用 C++/Qt 后端。

## 给后续 Codex 的提示

不要因为看见“Qt Bridge”就默认它指 Figma 或 Rust。执行前先判断当前任务是在处理设计导出还是语言绑定。未完成并记录上述 PoC 结果前，不得把 Qt Bridges for Rust 写入总纲的“已批准技术栈”。
