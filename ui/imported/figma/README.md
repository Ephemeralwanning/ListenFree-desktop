# Figma to Qt 资产入口

Figma to Qt 导出的 QML、资源和模块文件放在本目录，由导出流程覆盖更新。

业务逻辑、状态、模型绑定和内存/生命周期控制不得写入这里；请在 `ui/bootstrap`、后续 `ui/components` / `ui/pages` 或 C++ QML Bridge 中实现。
