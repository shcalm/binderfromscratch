# Android Binder 源码阅读基线

## 版本与证据范围

当前驱动阅读基线：上游 Linux **v6.6** 标签，`drivers/android/` 与 `include/uapi/linux/android/binder.h`。2026-11-06 核查了协议头，以及 binder.c 中本章涉及的事务入口和 BR 命令选择位置。

这不是 Android 平台的统一版本。运行基线也使用 Linux 6.6，已在 QEMU 的 TCG/KVM 中验证模块、mini-binder RPC 及真实 Binder 协议探测，证据见[阶段报告](../reports/m0-m1-progress.md)；这不等于完整 Android Binder RPC。Android 设备可能使用不同内核及厂商变更。用户态阅读另行固定 [AOSP Android 14 基线](aosp-userspace.md)，不能把 Linux 内核版本当作用户态平台版本，也不把阅读等同于完整 Android 构建验收。

| 来源 | 用途 |
| --- | --- |
| [binder.h](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/android/binder.h) | ioctl、事务数据、BC/BR 命令、对象类型 |
| [binder.c](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c) | 驱动入口、事务处理、对象/引用转换与工作交付 |
| [binder_internal.h](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder_internal.h) | 后续章节的内部结构对照入口，当前未逐项审读 |
| [binder_alloc.c](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder_alloc.c) | 后续 mmap/分配章节入口，当前未逐项审读 |

## 第 01 章已核查的入口

| 位置 | 本章应理解什么 |
| --- | --- |
| [binder_ioctl_write_read，5049 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L5049) | 读写协议缓冲区的 ioctl 处理入口 |
| [binder_thread_write，3801 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L3801) | 消费用户态发送的命令 |
| [BC_TRANSACTION / BC_REPLY，4014 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L4014) | 请求与回复进入事务处理的分支 |
| [binder_transaction，2896 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L2896) | 事务构造与目标、数据、对象处理的主要入口；本章不展开全部路径 |
| [binder_thread_read，4374 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L4374) | 向用户态交付待办工作 |
| [BR_TRANSACTION / BR_REPLY，4650 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L4650) | 交付请求还是同步回复 |
| [BR_TRANSACTION_COMPLETE，4486 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L4486) | 完成通知与业务回复是不同协议事件 |

行号只对这里链接的标签负责。后续更换版本时同时更新定位和分析，不能保留旧行号制造准确的假象。

## 对照阅读规则

1. 先用本章问题定位入口，再阅读相关分支；不要从全部结构体开始逐字段抄写。
2. 注明等价程度：概念对应、部分语义相同、ABI 不兼容或完全未实现。
3. 追踪数据、控制流、所有权三件不同的事。请求被接收不等于业务执行完成，回复完成也不等于接收缓冲区已释放。
4. 不将驱动的节点引用计数等同于用户态对象语言层生命周期，也不认为持有引用就能阻止服务进程退出。
