# 从零设计 Binder

从 `add(1, 2)` 出发，先遇到问题，再比较方案、做实验、引入机制，最终构建可运行的教学 IPC 驱动 mini-binder，并对照 Android Binder。

在线阅读：<https://shcalm.github.io/binderfromscratch/>。已提供 `main` 分支自动发布工作流，首次使用须完成[站点指南](docs/site-guide.md)中的仓库设置；各章仍按页面标记区分开发初稿与正式发布状态。

本教程以三个问题作为每章的验收标准：

1. 为什么要这样设计？原来的方案哪里不够，替代方案有什么代价？
2. 如何通过实验看见它工作，以及证明它在边界条件下仍然成立？
3. Android Binder 如何解决同一个问题，与教学版哪里相同、哪里不同？

## 从这里开始

- [教学设计规范](docs/design/teaching-contract.md)：全文必须遵守的推导和证据要求。
- [22 章内容与实现路线](docs/plans/roadmap.md)：逐章问题、实验和源码对照。
- [全项目任务总表](docs/plans/task-backlog.md)：全部章节、实验设施、可视化、网站和发布任务的统一进度入口。
- [第一阶段执行计划](docs/plans/phase-01.md)：当前状态、交付物、后续任务。
- [M0/M1 实现与验收记录](docs/reports/m0-m1-progress.md)：实测结果、环境哈希及未关闭的发布门槛。
- [P01：从 main.c 到 Linux 进程](docs/chapters/p01-process.md)：观察父子进程中的同址不同值。
- [P02：从函数调用到系统调用](docs/chapters/p02-syscalls.md)。
- [P03：第一个 Linux 内核模块](docs/chapters/p03-module.md)。
- [P04：搭建 Linux 内核实验环境](docs/chapters/p04-environment.md)。
- [第 01 章：为什么需要 Binder？](docs/chapters/01-why-binder.md)：用现有 IPC 实现一次 RPC，再讨论专用驱动的价值。
- [第 02 章：设计第一个字符设备驱动](docs/chapters/02-character-device.md)。
- [第 03 章：实现第一个同步 RPC](docs/chapters/03-sync-rpc.md)。
- [第 04 章：把请求放进只读接收区](docs/chapters/04-mmap.md)。
- [第 05 章：让双方一致地解释字节](docs/chapters/05-parcel.md)。
- [第 06 章：让一次调用只得到自己的回复](docs/chapters/06-transactions.md)。
- [第 07 章：同一个方法，究竟调用哪个对象](docs/chapters/07-nodes.md)。
- [第 08 章：把引用交给另一个进程，而不是复制一个整数](docs/chapters/08-references.md)。
- [Android Binder 源码基线](docs/reference/android-binder.md)：版本、源码入口与对照边界。

## 运行当前实验

需要 Linux、C 编译器、Make、Python 3；`strace` 用于可选观察。无需 root，也不会加载内核模块。

```sh
make test
./build/local_add
./build/fork_private
./build/socket_rpc 1 2
```

`make test` 编译入门、系统调用、映射及 C 模型程序，当前执行 101 项验收，包含共用 Parcel 库、独立编码 oracle、事务、Node 和 Ref 模型。各实验有截止时间；这不是 RPC 自身的超时协议。

运行驱动实验还需内核构建依赖和 QEMU，详见 P04：

```sh
make kernel
make busybox
make vm-test
make vm-test-kvm
make vm-runner-test
```

驱动只加载到 VM，宿主不需要加载模块。KVM 需要可用的 `/dev/kvm`；TCG 通过软件模拟运行。两者都已在本机验证。

## 当前进度

已有全书 22 章路线和 216 项任务分解（93 项勾选），P01–P04、第 01–08 章正文，以及查询设备、同步 RPC、只读映射接收、Parcel、双向事务、多对象 Node、私有 Ref/对象转换和 QEMU 验收路径。[映射](docs/reports/m2-mapping-progress.md)、[Parcel](docs/reports/m2-parcel-progress.md)、[事务](docs/reports/m2-transactions-progress.md)、[Node](docs/reports/m3-nodes-progress.md)和[Ref 报告](docs/reports/m3-references-progress.md)记录增量、实验及边界。正式章节发布、独立初学者复现仍待完成。第 09–18 章和电子书尚未交付。

按用户要求，本轮在 `dev-c08-20261106` 开发检查点暂停，先供阅读反馈；没有开始第 09 章。

本地 HTML 阅读站点的构建与预览见[网站指南](docs/site-guide.md)，未完成章节通过路线入口标记，代码和正文均为开发版本。

代码位于 `examples/`、`driver/` 和 `userspace/`，验收位于 `tests/`。章节持续演进同一个驱动；已提供[代码版本索引](docs/reference/code-versions.md)，正式阶段发布时采用不可移动的版本标签。
