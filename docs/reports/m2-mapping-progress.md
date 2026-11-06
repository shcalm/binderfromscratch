# M2：映射接收开发验收

日期：2026-11-06。本报告只覆盖第 04 章的映射接收，不表示第 05/06 章或整个 M2 已完成。设计决策见[只读接收区](../design/receive-region.md)，普通 C 实验见[映射模型](../labs/mapping-model.md)。前一版同步 RPC 固定为 `cfd7f42`；本次在它之上增量实现。

## 1. 验收的问题

上一版 CALL/RECEIVE 在发送者与接收者边界各复制一次请求数据。本版新增最长 4096 字节的 CALL_BUFFER，把 payload 直接 `copy_from_user` 到 Server 的内核可写、用户只读接收页；RECEIVE_BUFFER 只返回 token/offset/size。元数据与固定结果仍需复制，未声称零复制或吞吐提升。

用固定 64 KiB、16 页槽换取容易解释的分配规则；不是对真实 Binder allocator 的复刻。保留旧 add 命令与测试，不提供 Android libbinder ABI 兼容。映射请求、业务固定结果；变长结构化回复尚未实现。

## 2. 实际命令与环境

与 [M0/M1 环境](m0-m1-progress.md)使用同一 Linux 6.6 内核与 BusyBox 1.36.1，本次 initramfs SHA-256 为 `520b223aab30d222e5e354d86b649cad350b753597cf7f9cfd9615df695c92e4`。不同驱动阶段必须重新构建 rootfs，不能将旧快照模块与新 UAPI 混用。

```sh
make test
bash tools/qemu/build-rootfs.sh
LOG_TAG=mapping-final-tcg bash tools/qemu/run.sh
ACCEL=kvm LOG_TAG=mapping-final-kvm bash tools/qemu/run.sh
bash tools/qemu/test-runner.sh
```

用户态 **32 项通过**，含新增映射模型的 5 项；TCG 与 KVM 均返回 0。VM 先验证设备注册后失败回滚、接收区分配失败注入，再正常加载驱动执行查询、旧同步 RPC、新映射 RPC、独立 add 演示。所有正常路径均达到最终 MINI_LAB_PASS，卸载时 `live_contexts=0 requests=0 regions=0 slots=0`。

## 3. 覆盖矩阵

| 机制/反例 | 实际检查与结果 |
| --- | --- |
| 谁能建立映射 | 首次映射前 fork 子进程，继承 fd 的 mmap 返回 EPERM；父进程之后仍能成功映射。 |
| 范围与权限 | 私有、非零 offset、过长/过短、无读权限、写、执行映射均拒绝；mprotect 加写/执行失败。 |
| 真正的只读故障 | 探针自己 open/mmap、先读取零值，close fd 后仍可读取，再写入触发受控 SIGSEGV；不会把 DONTCOPY 后缺少 VMA 当成只读证明。 |
| VMA 与 fd | DONTCOPY 由 fork 后 mincore 的 ENOMEM 验证；局部 munmap 将 VMA 拆为 2，再逐个释放至 0；同上下文重新 mmap 拒绝。 |
| 模式错误 | 无映射注册返回 ENXIO；旧/新 Server 模式与 CALL 不匹配返回 EPROTOTYPE；没有默默入队等待错误协议。 |
| payload 传递 | 1、7、511、4096、3 字节分别按确定字节序列校验；每次验证 slot 尾部清零；累计 copies/bytes 与成功提交次数/字节数相符。 |
| 用户复制回滚 | 错误控制指针、错误 payload 指针、跨可读/PROT_NONE 页的部分复制失败；0/过长/保留字段非法均拒绝；槽、请求与成功复制计数不增加。 |
| 领取失败 | RECEIVE_BUFFER 输出地址 EFAULT，事务仍 queued，同一笔后续可领取并完成。 |
| 耗尽与恢复 | 逐个完成 16 笔调用但保留槽，第 17 笔返回 EAGAIN；FREE 一个后恢复；活跃 token/offset 不重复，新 token 不复用。 |
| 释放资格 | 同上下文交付后 FREE 成功；重复与 observer 错上下文 FREE 返回 ENOENT。 |
| 取消 | queued 中断后驱动自动释放槽；delivered 中断后槽保留、迟到 REPLY 返回 ENOENT，Server FREE 才回收。 |
| 回复与释放分离 | 正常回复后事务归零、槽仍占用；显式 FREE 后槽才归零。 |
| munmap 与退出 | Server munmap 后可继续用 fd FREE/REPLY；Server 最终退出时 queued/delivered 调用均 EPIPE。 |
| close 与映射持有 | Server 关闭 fd 但保留映射后仍 registered，可接受排队请求；最后移除映射并退出后 CALL EPIPE。 |
| 最终账目 | observer 的全局 live_regions/live_slots 等待至 0，query 上下文数恢复；卸载日志进一步核对全局零计数。 |

故意触发的用户态 SIGSEGV 会在串口出现 `mapping_test[...] segfault`，由父测试 waitpid 校验。它不是内核 Oops；core dump 已禁用。运行器仍拒绝 BUG/WARNING/Oops/panic，不能将任意段错误都解释为实验预期。

## 4. 失败到通过的证据

先在尚无 `.mmap` 的驱动加入设备映射断言，VM 因 `mmap` 返回 ENODEV 而失败；加入映射后通过。之后新增 `--allocation-failure` 断言，旧实现仍能分配，VM 确定失败；再加入只读模块参数 `fail_region_alloc=1`，使申请结果为 NULL，走相同 ENOMEM 分支。连续两次失败后检查未安装 VMA、没有 region/slot，卸载再正常加载，全部测试通过。

该注入验证受控失败分支及重试状态，不是耗尽宿主/客户机物理内存，不保证覆盖真实 vmalloc 部分失败的全部细节。注册后回滚实验也独立保留。

| 原始本地记录 | SHA-256 |
| --- | --- |
| `artifacts/mapping-red.txt` | `10665566f9da3b9ce7490e232fa55689a9d2dcabc03783b99495e63b05cb779d` |
| `artifacts/allocation-red.txt` | `0df4eabc95e04d34ac997d68f872b1ae68bf19675b807afa90a844ca9d204f00` |
| `artifacts/evidence/mapping-tcg.log` | `59850849941fe04adc85d86541177f55e84a933f275254ef941fb33a25fcfd26` |
| `artifacts/evidence/mapping-kvm.log` | `d764280197c65bb9b331612bd7abcf801f78f57557b7464cb72dfd93790ee750` |

日志在本工作区归档，尚未发布为外部下载包。稳定输入、构建路径和代码快照才是后续复跑入口，不能用摘要代替原始输出。

## 5. 代码与锁审查

独立静态复核未发现需修正的安全/功能问题。关键约束是 mb_lock 覆盖 reserve→输入复制→publish/rollback，防止复制目标在 Server release 中被销毁；region mutex 不跨用户复制；mmap/VMA 回调不取 mb_lock，避免与 mm 映射锁逆序。

queued 终结归还槽并清空接收上下文指针；交付时转移槽并清空该指针，后续取消与回复不再解引用 Server region。最终 release 先断开服务、终结依赖，再 vfree；不会让唤醒后的 Client 触碰已经释放的 Server。

这是实现与定向审查结论，没有运行 KASAN/lockdep，也没有穷尽线程交错。大锁跨用户复制仍可能被恶意缺页拖住，是明确保留的并发成本。

## 6. 剩余边界

- 尚未直接向 FREE_BUFFER 注入仍 queued 的有效 token 来动态验证 EBUSY，设计与代码分支存在，不能算该分支已实测。
- 尚未注入 remap 部分失败、mremap 的全部组合或 KASAN/lockdep；不将零告警等同于内存安全证明。
- 全局计数反映本模块账目，不是独立物理内存泄漏检测器；没有直接观测 PFN。
- 固定页槽浪费空间、单次映射不可重新建立、部分 munmap 后用户自行承担无效地址的使用后果。
- 没有结构化 Parcel、映射回复、多事务/线程路由、对象/FD 转换或死亡通知。
- 正式章节标签、独立初学者复现与完整 M2 阶段门槛仍待后续任务。

正常测试自行关闭/munmap 并回收子进程；失败丢弃 VM，由宿主运行器截止并终止 QEMU。无需也没有在宿主加载驱动。
