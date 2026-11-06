# 第 04 章设计决策：只读接收区

状态：开发实现已通过本机 TCG/KVM 验收，证据与边界见[阶段报告](../reports/m2-mapping-progress.md)，尚未正式发布。前版为 `cfd7f42`，保留旧 add RPC 命令及其全部测试。普通 C 共享映射实验独立于驱动；它验证映射属性，不替代以下 IPC 实现。

## 1. 问题与选择

第 03 章把 a/b 复制到内核事务，再用 RECEIVE 复制到 Server 的用户缓冲区。固定两个整数时成本很小；新需求是传入最长 4096 字节的请求。若继续复制接收，Server 需要准备容量、处理尺寸不足，并承担第二次 payload 复制。我们关注代码可数的复制点，尚无性能优势结论。

| 方案 | 可以解决什么 | 成本与选择 |
| --- | --- | --- |
| 提交后再复制给接收者 | 最容易管理，接收者拿到自己的私有数据 | payload 需要发送到内核、内核到接收者两次复制。保留为第 03 章对照。 |
| Client/Server 可写共享区 | 一份共享页就能交换字节 | 发送者可在校验后改写；多 Client 隔离、并发、分配与发布协议仍需设计。本章不选。 |
| 内核可写、接收者只读的接收区 | 驱动把发送 payload 直接复制到目标接收页，再交付偏移/长度 | 需要显式缓冲区释放、映射权限与 VMA 生命周期；选择此方案。 |

控制元数据仍通过 ioctl 复制，不能称为零复制。只读映射也不防止 Server 把数据另复制或自行 munmap；它约束用户态写入接收区的权限。

## 2. 最小接收区

每个打开上下文最多创建一次 64 KiB 映射，Linux x86_64 当前页大小 4096，16 个固定页槽，每槽容纳一笔 1–4096 字节请求。采用 `vmalloc_user` 预分配、清零并 `remap_vmalloc_range` 映射，先不模拟真实 Binder 的按需插页、区间分配和异步空间预算。页槽浪费空间是换取清晰边界的代价。

只接受 offset 0、完整长度、MAP_SHARED、PROT_READ；拒绝可写/可执行映射，清除后续加写/执行的能力。mmap 入口和 ioctl 一样核对打开者线程组，其他进程即使持有继承/转交的 fd 也不能抢先创建或读取该上下文的映射。设置 DONTEXPAND、DONTDUMP、DONTCOPY。fork 不复制接收 VMA，子进程持有继承 fd 也仍不能对父上下文 ioctl。此教学范围不支持同线程组共享文件但分离 mm 的非常规 clone 用法，后续安全章需继续审视 mm 绑定边界。

VMA 的文件引用使 close(fd) 后仍存活的映射保持上下文；最后一个 VMA 与 fd 都释放后才能进入最终 release。VMA 分裂/关闭用 open/close 回调计数；backing region 保留到最终上下文释放，不在 munmap 时立即 vfree。部分 munmap 后应用必须停止读取被移除的地址，内核不会替它保证返回偏移仍有用户映射。

本章不支持在同一上下文 munmap 后重新映射；需要重新 open。剩余 fd 可以释放已有缓冲，即使映射已全部撤销。所有映射和槽在最终 release 时回收。映射使服务文件多了一条持有路径，因此仅 close Server fd 不一定立即触发服务断开。

## 3. 增量接口

旧的 REGISTER/CALL/RECEIVE/REPLY 保持不变。新增能力位 `MB_FEATURE_MMAP`、独立的映射接收注册模式与以下命令；不同模式 CALL 直接返回 EPROTOTYPE，不能默默入队到不读取它的服务。

| 命令/数据 | 语义 |
| --- | --- |
| REGISTER_BUFFER | 当前上下文已有活跃接收映射，注册为唯一固定 Server；已有 Server 返回 EBUSY，没有映射返回 ENXIO。 |
| CALL_BUFFER | u64 用户数据地址、u32 长度、u32 保留字段；返回固定 u32 result/s32 status。输入长度 1–4096，保留字段必须为 0。 |
| RECEIVE_BUFFER | 返回 u64 请求 ID、u64 缓冲 token、u64 offset、u32 size、u32 保留字段；业务字节通过 `base + offset` 读取。 |
| FREE_BUFFER | 输入 u64 token；只有本上下文已交付的槽可以释放。未知/重复/其他上下文 token 返回 ENOENT；尚未交付返回 EBUSY。 |
| BUFFER_STATS | 本上下文已占用槽数、活跃 VMA 数、累计 payload 复制次数与字节数，以及全局 live_regions/live_slots；后两项允许另一个 observer 在目标最终释放后核对账目。并发读取不是跨所有资源的原子快照，验收应先建立静止同步点。 |

token 全局单调递增且不复用，回绕返回 EOVERFLOW；它不暴露内核地址。offset 只在本次接收区内有意义，不是跨进程用户指针。槽被释放后其字节不可继续使用，即使旧指针仍能被 CPU 读取；下一笔请求可以复用同一页。

此章只映射请求，回复保留上一章固定结果。第 05 章提出结构化变长回复时再扩展，不预装尚未需要的完整 Parcel 协议。

## 4. 状态与资源所有权

| 事件 | 事务 | 接收槽 |
| --- | --- | --- |
| 接收区满/输入复制失败 | 不进入全局事务列表；CALL 返回 EAGAIN/EFAULT | 输入失败撤销本次预留；先完整清零槽，防止短报文泄露旧内容。 |
| payload 复制成功并排队 | 由 CALL 的线程持有最终事务内存，记录接收上下文 | 槽由排队事务持有，不允许提前 FREE。 |
| RECEIVE_BUFFER 元数据复制失败 | 仍排队，可重新领取 | 仍由排队事务持有，无未交付泄漏。 |
| 成功交付 | 进入 delivered，可回复或取消 | 槽转交 Server 独立持有；事务取消或回复均不自动释放该槽。 |
| 排队期间 Client 取消 | 终结并回收事务 | 驱动撤销未交付槽。 |
| 交付后 Client 取消 | 终结并回收事务；迟到 REPLY 返回 ENOENT | Server 仍须 FREE_BUFFER；它可能仍在读数据。 |
| 正常回复 | 唤醒 CALL 并最终回收事务 | 可在回复前/后 FREE，两种事件没有隐含绑定。 |
| Server 最终文件释放 | 未完成事务返回 EPIPE | 未交付与已交付槽都由最终上下文清理；先结束依赖再销毁接收区。 |

每次成功 CALL_BUFFER 的 payload 只有一次 `copy_from_user`，直接落到目标 backing region；RECEIVE_BUFFER 只复制描述符。累计次数只在完整 payload 复制成功后增加，部分失败不能记作一次成功传输。计数用于证明代码路径与测试断言，不能替代吞吐测量。

## 5. 锁与生命周期约束

沿用睡眠 mutex `mb_lock` 保护 Server、事务队列和终结状态。预留槽 → 输入复制 → 入队或回滚的整段持有 mb_lock，阻止 Server 最终释放时销毁未入队复制目标；不能通过 CALL 持有 Server 的文件引用来代替，因为这可能反过来阻止断开唤醒。接收区自己的 setup mutex 只负责首次映射、槽及统计快照；VMA 引用计数独立。锁顺序允许 mb_lock → region mutex；region mutex 的持有者不能再取 mb_lock。

关键约束：`.mmap`/VMA 回调可能处于 mm 的映射锁内，不能再取 mb_lock。现有 RECEIVE 在 mb_lock 内执行用户复制，复制缺页可能需要 mm 读锁；若 mmap 同时持有 mm 写锁再等 mb_lock，就会形成死锁。新实现必须避免这一逆序，region mutex 也不能跨用户复制持有。大锁跨 copy 仍有恶意缺页阻塞其他调用的成本，保留为本章已知限制与第 15 章改进场景。

进入最终 release 时内核已经没有该文件的活动 ioctl/VMA 引用；仍需在 mb_lock 下断开服务并结束其排队请求，再释放区域。交付后事务不再依赖接收槽，不能保留可在 Server 退出后解引用的裸 region 指针。

## 6. 验收计划

新增 `tests/kernel/mapping_test.c`，纳入 rootfs/init；每个阶段由管道握手和 ioctl 计数确定先后，外层设截止，不用随机 sleep 证明竞争。

- 验证映射长度/offset/共享标志/写执行权限，以及 mprotect 升权拒绝；受控写进程收到 SIGSEGV，禁用 core dump。
- 用不同模式注册和调用证明错误能立即返回，而非等待不兼容 Server；保留全部旧设备/RPC 回归。
- 传入已知字节，比较映射内容、offset/length、页尾清零与统计值；一次成功 payload 计数 1，收到描述符后不增加。
- 验证坏用户指针/长度/保留字段、RECEIVE 输出失败回滚、满 16 槽后拒绝，FREE 后恢复；重复、错上下文与排队槽 token 不可释放。
- 分别控制 queued/delivered Client 中断、Server 最终退出与带 VMA 的 close；在每条结束路径核对事务和槽的不同账目。
- 验证 munmap 不立即释放 fd 持有的 backing region、close 后仍映射可读、最终退出资源回收。
- 分配失败用专用可控注入验收；没有实际触发的路径不填为通过。TCG/KVM 零告警只是本矩阵证据，不是任意交错的正确性证明。

## 7. 与真实 Binder 的对应边界

源码基线为 Linux v6.6：`binder_mmap` 限制映射入口，`binder_alloc_mmap_handler` 建立接收区，`binder_alloc_new_buf` 分配 buffer，`binder_alloc_copy_user_to_buffer` 写入接收页，`BC_FREE_BUFFER` 使用户态显式归还。实现前后直接核查这些符号及分支，不凭印象填行号。

真实 Binder 不是本章的 16 个固定 vmalloc 槽；映射建立、物理页存在、事务终结、用户缓冲释放分别是不同事件。它还支持回复缓冲、特殊对象、线程事务栈与更复杂的释放语义；本章只验证请求侧映射接收，并不兼容 Binder ABI。
