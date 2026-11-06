# M0/M1 实现与验收记录

记录日期：2026-11-06。本报告记录开发快照的实际观察，不宣布 M0/M1 正式发布。任务状态以[总表](../plans/task-backlog.md)为准；章节与代码对应见[版本索引](../reference/code-versions.md)。

## 1. 已实现的范围

P01–P04、第 01–03 章已有正文、C 实验、设计比较与固定 Linux v6.6 源码对照。当前驱动支持打开上下文、查询、固定 Server 注册、多 Client 同步 add RPC、请求关联、有界队列、中断及最终服务文件释放时的失败传播。第 02 章只查询设备的历史快照为 `6406d8d42e45358a9b289d080376ad858d4c2465`；RPC 版本是其后的增量开发，不能混用两版模块与用户态命令。

尚无映射接收、Parcel、Node/Ref、嵌套回调、FD 转换、死亡通知、oneway 或正式服务发现。Socket 示例用于揭示协议需求，不是教学驱动的替代实现。

## 2. 实测环境与输入

宿主为 Linux x86_64，GCC 11.4.0、GNU Make 4.3、QEMU 6.2。客户机为 Linux 6.6.0，512 MiB、2 vCPU；分别显式使用 TCG 与 KVM，关闭外部网络且不挂载宿主目录。默认 BusyBox 从固定 1.36.1 归档静态构建。内核与 BusyBox 归档校验、构建方式见[环境清单](../../tools/kernel/manifest.md)和 [P04](../chapters/p04-environment.md)。

| 产物 | 本次 SHA-256 |
| --- | --- |
| 内核 bzImage | `0a89e91f9df01bd2250d2dd11d62bc4b2fed7279c4c1c31cf46475f237950309` |
| 内核完整配置 | `8d1c658fd777db0dfd8a697d2c15a8c7d359f7086e2366a89d5889d347c59b81` |
| BusyBox 二进制 | `ceccdf1828870a4e1060dd196b712cd433e2536a691f3471ab0da334fa0be4c3` |
| BusyBox 完整配置 | `8af87795d08afe69a483636fcff997a09ad12295f38f87625a3dc6314338073f` |
| 本次 RPC initramfs | `d2b21f308701a79e3f39c4fe6fd24031c001c6475111c643bcd756f1a07d5ab8` |

这是当次产物身份，不是跨宿主字节一致的承诺。rootfs 使用新的临时目录组装并在退出时删除；内核和 BusyBox 的源码、配置及编译缓存仍会复用。本轮没有在第二个完全干净的源码/输出目录重新编译整个内核。

## 3. 用户态实验

执行 `make test`，结果为 **27 项通过、0 失败**。外层程序执行有截止时间；受控无回复与 readiness 负例有专用进程组清理，正常程序自行 waitpid 回收子进程。普通 subprocess.run 路径若因回归超时，只保证终止直接启动的进程，尚不能保证清理全部后代；统一的失败清理仍需完善。普通用户态实验不需要 root。

| 问题 | 实际覆盖 | 不能推出的结论 |
| --- | --- | --- |
| 进程与线程边界 | fork 私有变量；join 后线程写入可见；退出码 300 截为 44；SIGTERM；进程局部 RLIMIT_NPROC 触发 fork 的 EAGAIN | 没有直接观察物理页或完整页表；退出状态不是任意 RPC 返回值。 |
| RPC 协议 | 两进程 add；远端溢出；对端退出；8/16 字节错误报文；未知操作；活着不回复时由外部截止清理 | 外部测试截止不是 RPC 超时或取消协议。 |
| syscall 与错误 | 成功 write 保留旧 errno；失败捕获 EBADF；显式同步制造 EINTR，分别重试/停止 | 不代表所有 syscall 都有相同的重启语义。 |
| 命令分发 | 正常查询；完整命令的编号/方向/编码长度校验；容量不足；拒绝不改本地数组 | 本地 capacity 模型不能检查内核用户指针；真实 copy 失败可能已写入前缀。 |
| 请求管理 | 单槽覆盖丢请求、拒绝并重试、FIFO 交付、逆序回复关联、满队列与非法回复 | 单线程 C 模型没有验证真实调度、锁或内核生命周期。 |

strace 实际记录在 `artifacts/syscalls/`，实验的命令、判断与清理见[入门边界](../labs/intro-boundaries.md)、[系统调用](../labs/syscalls.md)及[队列模型](../labs/queue-model.md)。一次 readiness 行被拆分写入曾可能使测试卡住，现使用累计字节读取、单调截止和独立回归用例处理。

## 4. VM 集成与失败判定

本次执行：

```sh
bash tools/qemu/build-rootfs.sh
ACCEL=tcg bash tools/qemu/run.sh
ACCEL=kvm bash tools/qemu/run.sh
bash tools/qemu/test-runner.sh
bash tools/qemu/debug-boot.sh
```

两种加速方式均退出 0，包含 `BINDER_PROBE_PASS protocol=8 userspace_bits=64`、模块正常/失败/重载事件、`DEVICE_TEST_PASS`、`RPC_TEST_PASS concurrent=8 capacity=64 errors=validated requests=0`、`RPC_DEMO_PASS` 以及本次 `MINI_LAB_PASS`。驱动卸载记录 `live_contexts=0 requests=0`。原始串口流可能交错，不能把示例中的每行排版或 PID 写死为测试条件。

设备测试包含独立 open、dup、fork 继承权限、错误命令/编码长度、不可写用户页和上下文数恢复。RPC 测试通过控制管道和 STATS 建立排队/领取同步点，覆盖 8 Client 回复关联、容量 64 与第 65 笔拒绝、同上下文忙、无 Server、非法回复者/ID/重复回复、RECEIVE 复制失败后仍可领取、CALL 输出复制失败回收、业务溢出、排队及领取后的服务退出、Client 信号中断及 SIGKILL 清理。

尤其验收了 **带 SA_RESTART 的取消**：原实现直接返回 `-ERESTARTSYS` 会让 ioctl 被重启，重新提交一次业务请求。先观察到回归测试失败，再将未完成 CALL 的中断转换为 `-EINTR`，重复测试通过；迟到回复返回 ENOENT。已完成与信号竞争时，以锁内观察到的完成结果优先。这个规则不撤销 Server 已经执行的副作用，也不提供 exactly-once 保证。历史失败日志末尾的 errno 可能是旧值，不能据此把原因归为 EBADF。

运行器负例结果：assert、no-marker、panic 均被拒绝并退出 1；hang 超时退出 124；缺少内核镜像的启动失败也被拒绝。负例使用独立日志文件，不覆盖正常路径证据。GDB 在 `start_kernel` 命中硬件断点并取得函数栈；当前 vmlinux 没有 DWARF，不能声称已验收源码行调试。

清理方式：正常 VM 内卸载模块并关机；失败后丢弃 VM；宿主运行器以 timeout 限制 QEMU，调试脚本 trap 终止并回收自己的 VM。没有向宿主内核加载教学模块。`make clean` 删除用户态构建程序，保留内核及 BusyBox 缓存。

## 5. 本地证据归档

以下文件位于忽略的 `artifacts/evidence/`，是本工作区原始证据，尚未作为外部下载包发布。将来发布必须同时归档代码快照、输入清单、许可证与日志，不能只依赖此表中的哈希。

| 文件 | SHA-256 |
| --- | --- |
| `query-tcg.log`（旧查询设备，BusyBox 1.30.1） | `63081d7271cbc4dd14560cbbd05d1f744320952426da20d700b460af07006776` |
| `query-kvm.log`（同上） | `c06917c88e71c54f142150f23a6be9ca7729a8969fb296cfa08263f71faa1c6f` |
| `rpc-tcg.log`（本次） | `9be7b2238b782af94c155da079a41c97b0b2a0cbbde955dab4000e722ba598b5` |
| `rpc-kvm.log`（本次） | `f2a71fffa210aefebdfa754bd3219dd09ccf9e2bb6e7c01044d6d1c1fed4cd6d` |
| `runner-negative.txt` | `66de249d2516efc3efaea3a823faf71bbe85a47b4e53dfbbb4dcad9f38128073` |
| `gdb-session.log` | `f158a853d1f19ed3761f360867c6f39623ac7577a0ab2475e98e45138c3a880b` |
| `restart-red.log` | `425206581de557bf5f7b1446273ce65158b40310ba41fbfdbdcfbc003d40facc` |
| `restart-green.log` | `0c1bfb889985cd9da09c27f46890e66677cf512347e7b88f71d6d4e704d60200` |

## 6. Android 对照的实际边界

真实 Linux v6.6 Binder 已编入当前内核；init 挂载 `/dev/binderfs`，探测程序使用从同一源码导出的 UAPI 打开 `/dev/binderfs/binder` 并执行 BINDER_VERSION，确认 64 位协议版本 8。该探测与 `/dev/mini_binder` 的自有 ABI 1 是两条独立路径。

目前只有真实驱动协议探测，没有真实 Binder 服务注册、对象传递或完整请求回复实验。AOSP 用户态源码版本还未锁定，不能把这一结果称为 Android libbinder/ServiceManager 验收。早期章节的实际源码入口及简化在各章对照表中记录。

## 7. 尚未关闭的阶段门槛

- G-08/R-02：补齐 RPC 固定代码快照、正式不可移动章节标签及对应 diff。
- P04-03/P04-07：预构建产物及许可证/源码配套发布、从独立干净目录完整重建。
- P04-04/C02-05：模块版本不匹配、缺少配置、设备注册失败和分配失败的受控注入；当前已有的是 hello 初始化主动失败，不等同于这些路径。
- 各章末尾验收任务与 R-08：独立初学者复现及三项教学标准完整复核。自动检查不能替代真实读者反馈。
- E-07 至 E-14：当前只有 RPC/上下文的部分日志、计数、同步和复制故障覆盖；未来缓冲区/Node/Ref/FD/通知、调试内核、压力与性能方法仍需逐阶段补齐。
- W/V/R：网页渲染、可视化、正式阶段发布和电子书尚未交付。

当前可以继续验证 M2 设计，不应据此把以上待验收项勾选为完成。以下补充记录会明确关闭其中的具体缺口，原始快照与日志保持不变。

## 8. 后续补充：独立干净构建与设备回滚

同步 RPC 开发快照现固定为 `cfd7f42`，可用 `git diff 6406d8d cfd7f42 -- driver/mini_binder.c include/uapi/mini_binder.h` 阅读第 02→03 章变化。正式发布标签仍未建立。

在 `build/clean-replay` 创建该提交的独立 checkout，只复制并重新校验两份原始下载归档，不复用源码目录、完整配置、编译输出、模块、BusyBox 或 rootfs。实际运行：

```sh
git worktree add --detach build/clean-replay cfd7f42
mkdir -p build/clean-replay/build/kernel build/clean-replay/build/busybox
cp build/kernel/linux-6.6.tar.xz build/clean-replay/build/kernel/
cp build/busybox/busybox-1.36.1.tar.bz2 build/clean-replay/build/busybox/
cd build/clean-replay
bash tools/kernel/build.sh
bash tools/qemu/build-busybox.sh
bash tools/qemu/build-rootfs.sh
ACCEL=tcg bash tools/qemu/run.sh
ACCEL=kvm bash tools/qemu/run.sh
```

全部构建退出 0，TCG/KVM 均完成模块正常/主动失败/重载、设备、RPC、独立演示与最终零计数。该证据关闭“同一宿主上，从干净源码/输出目录重建”的缺口，不等于在新安装的宿主或真实初学者环境验证。

干净构建内核 SHA-256 为 `d8d3c0d6c7e4a674588e6890eec8ab9aca213846d85054f64ead77042c1cc2bd`，完整内核配置与上表相同。BusyBox 为 `59f839dfdfc15368b173d6f273923981b06c303930e784b8d521f5ed427eb58a`，initramfs 为 `fbe16be3109008c91372052b41b9693047cdf1b9f8a5022391104805dbb6c114`。BusyBox 配置文件哈希与上表不同，实际 diff 只有自动生成时间注释；这恰好说明文件哈希、配置语义与功能等价是不同判断。

归档日志 `artifacts/evidence/clean-replay-tcg.log` 的 SHA-256 为 `e5eb889f26e7a8dfb96453bd9a02ae8b19d0d82e33b2c92fdbeca1eddd6ec021`，KVM 为 `c880a82235d2c0eda8d550aa17af60fb6b886264087c404ec9ba66d9525fd768`。

主线在该快照之后补入 `fail_after_register=1` 模块参数：先注册 miscdevice，再主动撤销并返回 EIO。VM 验证装载失败后设备节点和 `/proc/modules` 条目均不存在，且出现本次 rollback 日志；随后正常加载，设备/RPC/独立演示仍通过。先运行新测试对旧实现观察到失败，再加入回滚实现通过。两次宿主完整记录为 `artifacts/device-rollback-red.txt`（SHA-256 `4fe63a41cf3277a0cee3d75f632fc96e58e4add311cb247524dc9c4115a03441`）和 `artifacts/device-rollback-green.txt`（`1a90a300fb369fd9472f132706960150cf2f201d9f652b12328c59b1a95b34c1`）。

这验证的是“注册成功后后续初始化失败”的回滚，不是让 misc_register 本身实际返回失败，也不是物理内存耗尽。后续 mmap 版本继续保留此检查；具体镜像不能与前面历史 RPC 哈希混用。
