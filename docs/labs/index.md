# 实验入口

本页汇集已有实验；准确命令、预期现象、失败判据和清理方法由链接的实验手册与章节维护。宿主命令在仓库根目录执行；QEMU 手动手册会明确区分宿主与虚拟机命令。

想亲手进入虚拟机操作驱动，从[在 QEMU 里手动验证 mini-binder](qemu-manual.md)开始：启动 shell、加载模块、运行 Server/Client、观察成功回复与错误、逐项测试并关机。

| 内容 | 前置环境 | 运行入口 | 预期证据与边界 |
| --- | --- | --- | --- |
| [P01 / 01 进程与 Socket RPC](intro-boundaries.md) | Linux、C、Make、pthread、Python 3；无需 root | `make test`；手册中的单项程序命令 | 父子私有值、线程 join、协议错误和受控无回复；仅用户态模型 |
| [P02 系统调用](syscalls.md) | 上述环境；跟踪另需 strace | `make test`；`strace -f` 命令见手册 | 返回值/errno、信号中断、Socket RPC trace；不是 mini-binder 驱动实验 |
| [P03 / P04 模块与环境](../chapters/p04-environment.md) | 固定内核/BusyBox、构建依赖、QEMU；KVM 可选 | `make kernel`、`make busybox`、`make vm-test` | VM 内装卸、回滚、协议探测及汇总标记；驱动只加载到 VM |
| [QEMU 手动实验](qemu-manual.md) | 当前开发版 P04 环境；KVM 或 TCG | `make rootfs`，再按手册启动交互 shell | 手动加载设备、四类调用结果、逐项测试及卸载回收 |
| [02 查询设备](../chapters/02-character-device.md) | P04 环境；使用该章固定提交自己的构建脚本 | 正文中的独立工作树和 VM 命令 | 查询/错误路径及上下文归零；不能混用最新 RPC 镜像 |
| [03 请求队列模型](queue-model.md) | 用户态构建环境 | `make test`；`./build/request_queue` | 单槽覆盖与 FIFO/回复关联对照；不模拟线程调度 |
| [03 同步 RPC](../chapters/03-sync-rpc.md) | 当前开发版 P04 环境、QEMU | `make vm-test`；可用时 `make vm-test-kvm` | 并发、退出、中断与资源回收；本章范围为同步 RPC |
| [04 接收映射模型](mapping-model.md)与[驱动正文](../chapters/04-mmap.md) | 当前主线 P04 环境、QEMU | `make test`、`make vm-test`；可用时 `make vm-test-kvm` | 映射及生命周期边界；实际判据与限制见正文和[映射报告](../reports/m2-mapping-progress.md) |
| [05 Parcel 模型](parcel-model.md)与[驱动正文](../chapters/05-parcel.md) | 用户态模型无需设备；驱动实验需 P04 环境 | `make build/parcel`、`make test`、`make vm-test` | 独立字节 oracle、55 条映射消息、驱动/业务两层拒绝及回滚；所有特殊对象拒绝 |
| [06 事务模型](transaction-model.md)与[驱动正文](../chapters/06-transactions.md) | 用户态模型无需设备；驱动实验需 P04 环境 | `make build/transactions`、`make test`、`make vm-test` | 单槽反例、52 个状态/事件、同 fd 反序回复、双向槽和取消/退出；[报告](../reports/m2-transactions-progress.md)列出竞争边界 |
| [07 Node 模型](node-model.md)与[驱动正文](../chapters/07-nodes.md) | 用户态模型无需设备；驱动实验需 P04 环境 | `make build/nodes`、`make test`、`make vm-test` | 多对象同方法、三类持有、跨进程反序业务与退出；[报告](../reports/m3-nodes-progress.md)明确全局 ID 和本地对象边界 |
| [08 Ref 模型](reference-model.md)与[驱动正文](../chapters/08-references.md) | 用户态模型无需设备；驱动实验需 P04 环境 | `make build/references`、`make test`、`make vm-test` | raw Handle 反例、真实三进程转换、两类 Ref 单位、前缀回滚及死亡对象清理；[报告](../reports/m3-references-progress.md)明确教学 grant 和未测交错 |

## 第 09–18 章如何阅读与观察

后续正文已补齐，mini 驱动尚未加入相应扩展。可以先用文内普通示例检验基本机制，再按固定版本源码和设计场景追踪责任。

| 正文 | 当前可以做什么 | 需要继续实现/实测的部分 |
| --- | --- | --- |
| [09 ServiceManager](../chapters/09-service-manager.md) | 按注册、查询、对象转交场景逐步列出名字表和私有 Handle | 多 owner 路由、管理器引导、服务注册与查找协议 |
| [10 线程](../chapters/10-thread-model.md) | 对照 A→B→A 的调用栈，阅读真实线程待办与等待循环 | mini 线程池、嵌套路由、线程退出与并发压测 |
| [11 FD](../chapters/11-file-descriptors.md) | 在 Linux 上运行文内完整 SCM_RIGHTS 对照，观察同一打开文件的偏移 | mini FD 转换、接收资格、安装与失败回滚 |
| [12 生命周期](../chapters/12-lifetime.md) | 对照 C08 两类单位与拟议强弱协议，逐事件检查死亡通知状态 | 强弱持有协作、死亡注册/取消/交付/确认竞态 |
| [13 安全](../chapters/13-security.md) | 核对可信身份的来源、版本及用户态业务授权边界 | 跨 UID 策略、namespace 实测、对象/FD 转交权限矩阵 |
| [14 Oneway](../chapters/14-oneway.md) | 推演慢消费者和有界队列，分别记录提交、执行和资源归还 | 异步队列、Node 顺序、背压及退出清理 |
| [15 并发](../chapters/15-concurrency.md) | 审读现有锁顺序和资源持有，设计可以复现的退出/复制故障场景 | 新功能压力循环、KASAN/lockdep、分配失败矩阵 |
| [16 架构](../chapters/16-binder-architecture.md)与[17 调用链](../chapters/17-call-trace.md) | 用固定源码逐事件定位 proc/thread/node/ref/alloc、请求/回复/释放 | 完整 Android 服务运行、真实调用 trace 与端到端故障日志 |
| [18 复盘](../chapters/18-design-review.md) | 运行已存在的代表性反例，检查版本化 C/Rust 边界 | 额外语言实现的构建、运行与兼容验收 |

每章中的“预期”或“设计推演”是要检验的命题；[本轮正文报告](../reports/remaining-chapters-draft.md)只记录实际执行过的验证。普通 Socket 文件传递不构成 mini FD 支持，源码阅读也不构成真实 Android 调用实测。

## 版本、标签与差异

[代码版本索引](../reference/code-versions.md)区分 `6406d8d42e45358a9b289d080376ad858d4c2465` 的查询设备、`cfd7f42` 的同步 RPC、`dev-c04-20261106` 的映射、`dev-c05-20261106` 的 Parcel、`dev-c06-20261106` 的事务、`dev-c07-20261106` 的 Node 和 `dev-c08-20261106` 的 Ref 开发快照。目前没有正式章节发布标签；开发标签不等于通过整章教学门槛。已有实测环境与产物摘要见[阶段报告](../reports/m0-m1-progress.md)。

在本地仓库查看从查询设备到当前开发状态的变化（包含已跟踪文件的未提交修改）：

```sh
git show 6406d8d42e45358a9b289d080376ad858d4c2465:driver/mini_binder.c
git diff cfd7f42 dev-c04-20261106 -- driver/ include/uapi/ tests/kernel/mapping_test.c
git diff dev-c04-20261106 dev-c05-20261106 -- driver/ include/uapi/ userspace/ tests/kernel/parcel_test.c
git diff dev-c05-20261106 dev-c06-20261106 -- driver/ include/uapi/ tests/kernel/transaction_test.c
git diff dev-c06-20261106 dev-c07-20261106 -- driver/ include/uapi/ tests/kernel/node_test.c
git diff dev-c07-20261106 dev-c08-20261106 -- driver/ include/uapi/ tests/kernel/ref_test.c examples/models/references.c
git diff 6406d8d42e45358a9b289d080376ad858d4c2465 -- driver/ include/uapi/
git status --short
```

`git diff` 不列出未跟踪的新文件；当前完整工作树源码可从[源码阅读入口](../source/index.md)逐项查看。这里没有虚构远程仓库或已发布 diff 页面；正式标签和前后版本发布链接仍由版本索引后续补齐。
