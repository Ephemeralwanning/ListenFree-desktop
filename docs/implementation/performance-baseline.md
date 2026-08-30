# Bootstrap 性能基线

## 测量环境

- OS：Windows（当前开发机）
- 架构：x64
- Qt：6.10.3 MinGW
- 构建：Release
- 场景：启动 Bootstrap QML，使用 `--platform offscreen`，稳定约 1.5 秒后采样；另以 `--smoke` 测量启动到自动退出
- 指标：主进程 `PrivateMemorySize64`、`WorkingSet64`、句柄数
- 采样方式：PowerShell `Get-Process`；进程树当前只有主程序，SourceHost 未在此场景加载

## 实测样本

| 场景 | Private Memory | Working Set | Handles | 退出/耗时 |
|---|---:|---:|---:|---:|
| Release 空闲启动（单次） | 27.28 MiB | 38.75 MiB | 250 | 手动结束采样 |
| Release Smoke 启动退出（单次，当前回归） | 不采样 | 不采样 | 不采样 | 14 ms，退出码 0 |

这是空骨架基线，不代表完整产品的内存结论。当前场景没有加载 SourceHost，进程树只有主程序；启动时间为单次本地回归样本，不能作为最终冷启动门槛。M5 需要在同一设备上至少重复三次，并增加 SourceHost 已加载、播放本地音频和歌词场景，报告进程树中位数与最大值。
