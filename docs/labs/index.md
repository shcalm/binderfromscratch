# 实验入口

本页汇集已有实验；准确命令、预期现象、失败判据和清理方法由链接的实验手册与章节维护。所有命令在仓库根目录执行。

| 内容 | 前置环境 | 运行入口 | 预期证据与边界 |
| --- | --- | --- | --- |
| [P01 / 01 进程与 Socket RPC](intro-boundaries.md) | Linux、C、Make、pthread、Python 3；无需 root | `make test`；手册中的单项程序命令 | 父子私有值、线程 join、协议错误和受控无回复；仅用户态模型 |
| [P02 系统调用](syscalls.md) | 上述环境；跟踪另需 strace | `make test`；`strace -f` 命令见手册 | 返回值/errno、信号中断、Socket RPC trace；不是 mini-binder 驱动实验 |
| [P03 / P04 模块与环境](../chapters/p04-environment.md) | 固定内核/BusyBox、构建依赖、QEMU；KVM 可选 | `make kernel`、`make busybox`、`make vm-test` | VM 内装卸、回滚、协议探测及汇总标记；驱动只加载到 VM |
| [02 查询设备](../chapters/02-character-device.md) | P04 环境；使用该章固定提交自己的构建脚本 | 正文中的独立工作树和 VM 命令 | 查询/错误路径及上下文归零；不能混用最新 RPC 镜像 |
| [03 请求队列模型](queue-model.md) | 用户态构建环境 | `make test`；`./build/request_queue` | 单槽覆盖与 FIFO/回复关联对照；不模拟线程调度 |
| [03 同步 RPC](../chapters/03-sync-rpc.md) | 当前开发版 P04 环境、QEMU | `make vm-test`；可用时 `make vm-test-kvm` | 并发、退出、中断与资源回收；本章范围为同步 RPC |
| [04 接收映射模型](mapping-model.md)与[驱动正文](../chapters/04-mmap.md) | 当前主线 P04 环境、QEMU | `make test`、`make vm-test`；可用时 `make vm-test-kvm` | 映射及生命周期边界；实际判据与限制见正文和[映射报告](../reports/m2-mapping-progress.md) |
| [05 Parcel 模型](parcel-model.md)与[驱动正文](../chapters/05-parcel.md) | 用户态模型无需设备；驱动实验需 P04 环境 | `make build/parcel`、`make test`、`make vm-test` | 独立字节 oracle、55 条映射消息、驱动/业务两层拒绝及回滚；所有特殊对象拒绝 |
| [06 事务模型](transaction-model.md)与[驱动正文](../chapters/06-transactions.md) | 用户态模型无需设备；驱动实验需 P04 环境 | `make build/transactions`、`make test`、`make vm-test` | 单槽反例、52 个状态/事件、同 fd 反序回复、双向槽和取消/退出；[报告](../reports/m2-transactions-progress.md)列出竞争边界 |

## 版本、标签与差异

[代码版本索引](../reference/code-versions.md)区分 `6406d8d42e45358a9b289d080376ad858d4c2465` 的查询设备、`cfd7f42` 的同步 RPC、`dev-c04-20261106` 的映射、`dev-c05-20261106` 的 Parcel 和 `dev-c06-20261106` 的事务开发快照。目前没有正式章节发布标签；开发标签不等于通过整章教学门槛。已有实测环境与产物摘要见[阶段报告](../reports/m0-m1-progress.md)。

在本地仓库查看从查询设备到当前开发状态的变化（包含已跟踪文件的未提交修改）：

```sh
git show 6406d8d42e45358a9b289d080376ad858d4c2465:driver/mini_binder.c
git diff cfd7f42 dev-c04-20261106 -- driver/ include/uapi/ tests/kernel/mapping_test.c
git diff dev-c04-20261106 dev-c05-20261106 -- driver/ include/uapi/ userspace/ tests/kernel/parcel_test.c
git diff dev-c05-20261106 dev-c06-20261106 -- driver/ include/uapi/ tests/kernel/transaction_test.c
git diff 6406d8d42e45358a9b289d080376ad858d4c2465 -- driver/ include/uapi/
git status --short
```

`git diff` 不列出未跟踪的新文件；当前完整工作树源码可从[源码阅读入口](../source/index.md)逐项查看。这里没有虚构远程仓库或已发布 diff 页面；正式标签和前后版本发布链接仍由版本索引后续补齐。
