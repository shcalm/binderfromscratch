# 从零设计 Binder

从 `add(1, 2)` 出发，通过问题、方案比较与实验构建教学 IPC 驱动，再对照固定版本的 Android Binder。

**当前是开发阅读版。** P01–P04 和第 01–08 章有正文及已有实验；第 09–18 章已补齐设计与源码阅读稿，可以连续阅读。mini-binder 的实际实现仍到第 08 章，后续章节会标明哪些是设计推演、普通程序观察或源码结论。全书尚未完成正式章节验收。

本轮 review 从[第 09 章：如何找到第一个服务](chapters/09-service-manager.md)开始；[全书阅读顺序](reading-path.md)列出每章的具体问题和阅读状态。

| 阅读顺序 | 内容 | 状态 |
| --- | --- | --- |
| 预备篇 | [P01 进程](chapters/p01-process.md) → [P02 系统调用](chapters/p02-syscalls.md) → [P03 内核模块](chapters/p03-module.md) → [P04 实验环境](chapters/p04-environment.md) | 初稿；执行模块实验前先准备 P04 环境 |
| 第一篇 | [01 IPC 动机](chapters/01-why-binder.md) → [02 字符设备](chapters/02-character-device.md) → [03 同步 RPC](chapters/03-sync-rpc.md) | 初稿；尚未正式发布 |
| 第二篇 | [04 mmap 接收映射](chapters/04-mmap.md) → [05 Parcel 请求](chapters/05-parcel.md) → [06 Transaction](chapters/06-transactions.md) | 开发初稿；尚未正式发布 |
| 第三篇 | [07 Node 对象身份](chapters/07-nodes.md) → [08 Ref 与对象转交](chapters/08-references.md) → [09 ServiceManager](chapters/09-service-manager.md) | 07–08 为开发初稿；09 为设计稿 |
| 第四篇 | [10 线程与回调](chapters/10-thread-model.md) → [11 文件描述符](chapters/11-file-descriptors.md) → [12 生命周期](chapters/12-lifetime.md) | 设计与源码阅读稿 |
| 第五篇 | [13 身份与授权](chapters/13-security.md) → [14 Oneway](chapters/14-oneway.md) → [15 并发与回收](chapters/15-concurrency.md) | 设计与源码阅读稿 |
| 第六篇 | [16 真实架构](chapters/16-binder-architecture.md) → [17 完整调用链](chapters/17-call-trace.md) → [18 设计复盘](chapters/18-design-review.md) | 固定版本源码阅读与复盘稿 |

运行前先看[实验入口](labs/index.md)和[代码版本索引](reference/code-versions.md)。章节中的源码链接展示本次站点构建时的工作树，旧章节快照须按版本索引核对。

[教学规范](design/teaching-contract.md)说明论证标准，[范围声明](design/scope.md)说明教学简化，[M0/M1 阶段报告](reports/m0-m1-progress.md)、[映射报告](reports/m2-mapping-progress.md)、[Parcel 报告](reports/m2-parcel-progress.md)和[事务报告](reports/m2-transactions-progress.md)记录实际证据及边界。逐项完成状态以[任务总表](plans/task-backlog.md)为准。
