# P02｜从函数调用到系统调用

上一章已经运行了 [本地 add](../../examples/intro/local_add.c)、fork 私有变量和线程共享变量实验。第 01 章还有 [Socket RPC](../../examples/intro/socket_rpc.c)：一个进程发请求，另一个进程执行加法，再把回复发回来。

现在先问一个更基础的问题：普通 C 函数能操作自己的变量，谁有权创建 Socket、读取另一个执行实体送来的数据、让当前线程等待？如果把 `send()` 换成一个普通的 `memcpy()`，接收方为什么不会因此得到请求？

本章补齐进入内核的接口、错误约定和等待语义。完整可执行代码在 [examples/syscalls](../../examples/syscalls/)，复现说明在 [系统调用实验](../labs/syscalls.md)。本章不实现 mini-binder 驱动；下一步才定义设备接口。

## 1. 普通函数、库函数与系统调用

`add(1, 2)` 可以只修改寄存器和本进程内存。编译器甚至可能直接算出 3，不产生独立函数调用。它不需要内核替自己管理另一个进程的资源。

`write(fd, buffer, size)` 则请求内核向一个打开的资源写入数据。用户态程序不能通过直接修改内核中的文件表或 Socket 队列来完成这件事；系统调用提供受控入口，让内核检查描述符、参数和资源权限，再执行相应操作。

这里有三种容易混淆的层次：

| 名称 | 谁执行主要工作 | 例子与限制 |
| --- | --- | --- |
| 普通函数 | 当前程序中的用户态代码 | `add` 可以被内联，不一定进入内核 |
| C 库接口 | 用户态库，必要时进入内核 | `printf` 可能先缓冲，多次调用不等于多次 `write` |
| 系统调用 | 内核入口及其处理路径 | libc 的 `write` 包装器通常进入同名系统调用；包装器名字不保证与底层入口一一对应 |

例如，本机 `fork()` 在 strace 中显示为 `clone`，`pipe()` 显示为 `pipe2`。这不表示实验换了程序，而是库接口与底层 ABI 可以不同。直接调用 `syscall()` 也不是本章的必要条件；通常使用 libc 提供的接口就足够。

系统调用意味着当前线程从用户态进入内核态，再返回用户态。它不必切换到另一个进程。操作立即完成时，同一个线程可以一路执行回来；需要等待数据时，内核才可能让线程睡眠，并调度其他可运行任务。是否切换、切换给谁，不能靠“出现了一条系统调用日志”推断。

## 2. 先看返回值，再看 errno

运行：

```sh
make
./build/errno_write
```

本机预期：

```text
event=write_success return=1 errno=33 decision=success
event=payload_read return=1 value=X
event=write_failure return=-1 errno=9 decision=stop
```

[errno_write.c](../../examples/syscalls/errno_write.c) 先创建管道，故意把 `errno` 设为 `EDOM`，再写入一个字节。写入返回 1，读端确实读到 X；本次 Linux/glibc 包装器仍留下旧的 `errno=33`。如果写成 `if (errno != 0) error();`，就会把成功写入误判为失败。

`errno` 不是自动清零的“最近一次操作是否成功”指示灯。对 `write` 应先检查返回值：负数表示失败，才读取并解释 errno；非负数表示实际写入的字节数，可能少于请求长度。普通通用函数还可能有不同约定，例如 pthread 接口常直接返回错误码，不能把这一规则机械套到所有 C API。

第二次调用使用 -1 描述符，`write` 返回 -1，捕获的 errno 是 `EBADF`。必须立即保存 errno，再调用可能改变它的打印、关闭或其他库接口。这里的 33、9 是本机数值，代码与测试使用的是 `EDOM`、`EBADF` 符号。

成功调用未清除 EDOM 是本次观察；可移植规则是“成功后不靠 errno 判断失败”，而不是“所有成功调用都保证 errno 原样保留”。也不需要在每次调用前清零 errno 来弥补错误的判断方式。

## 3. 阻塞不是不断重试

如果接收端需要等数据，可以比较三种方案：

| 方案 | 能解决什么 | 代价或不足 |
| --- | --- | --- |
| 循环查询非阻塞 fd | 可以随时检查退出条件 | 空转耗 CPU；需要明确 EAGAIN 和重试策略 |
| 定期 sleep 后再查 | 降低查询频率 | 人为延迟，sleep 不证明对方已经进入某个阶段 |
| 阻塞等待可读条件 | 条件未满足时让线程休眠 | 必须定义中断、退出、EOF 与资源清理 |

本章选择第三种，但把等待和读取分开。`pselect` 等待管道可读，随后 `read` 取出数据。`pselect` 返回正数只表明有 fd 就绪，不保证某个业务结果已经完成；在我们的单读者管道中，写入一个字节后就能读取它。真实程序还要考虑其他读者抢先读取、EOF、描述符状态等情况。

为了观察中断，[interrupt_wait.c](../../examples/syscalls/interrupt_wait.c) 增加一个控制管道。父进程负责发信号和数据，子进程负责等待。这个控制管道只是实验设施，不是业务 RPC 协议。

## 4. 用握手稳定地得到 EINTR

如果只让子进程打印“将要 read”，父进程收到文字后立即发信号，信号可能在 read 开始前就已处理完。随后 read 仍然等待，这不能证明“信号没有中断能力”。再加一个 sleep 也只能降低概率，不能排除竞态。

本实验采用以下状态顺序：

1. fork 前安装 SIGUSR1 处理函数，并屏蔽 SIGUSR1。处理函数只设置 `volatile sig_atomic_t` 标志。
2. 子进程打印 `event=waiting`，通过控制管道发送 R；父进程收到 R 才发送 SIGUSR1。
3. 子进程调用 `pselect`，原子地临时解除 SIGUSR1 屏蔽并等待空数据管道。即使信号早已挂起，也不会在“检查完标志、尚未等待”的缝隙中丢掉它。
4. pselect 被处理的信号中断，返回 -1、errno 为 EINTR。子进程确认这一结果后才发送 I。
5. 父进程收到 I 后，根据模式决定写入 X 或直接结束；因此数据绝不会抢在中断观察之前抵达。

`--retry` 模式输出：

```sh
./build/interrupt_wait --retry
```

```text
event=waiting pid=<child-pid>
event=interrupted return=-1 errno=4 signal_seen=1 action=retry
event=read_complete return=1 value=X interruptions=1
```

本例的等待只查询就绪条件，没有消耗业务字节，因此选定策略允许在 EINTR 后重新建立 fd 集合再等待。其他错误不会进入无限重试。

同一个中断也可以被解释为取消：

```sh
./build/interrupt_wait --stop
```

最后两行变成 `action=stop` 和 `event=cancelled interruptions=1`，不读取 X。两种模式都以 0 退出，表示实验按指定策略结束。EINTR 本身不替应用决定重试还是取消。

这不是所有 `read/write/ioctl` 的通用重试模板。正数返回值可能已经表示部分工作完成；非幂等业务不能不经分析就整笔重发。某些系统调用在特定信号设置下会自动重启，某些等待接口不会；应阅读所用接口的约定。尤其不要把 `close` 的 EINTR 处理照搬为重复关闭同一个数字 fd，Linux 上该数字可能已被释放并复用。

## 5. 用户指针、VFS 与驱动入口

为了下一章的设备接口，现在只补三项概念。

第一，用户态传入的 buffer 是当前调用者地址空间中的地址。内核不能因为它有 `void *` 类型就认为地址合法，也不能长期借用这块内存而不定义生命周期。后续驱动通过 `copy_from_user`、`copy_to_user` 等接口访问，并处理复制失败；这些接口也不意味着整个应用协议自动完成校验。

第二，fd 是进程文件描述符表中的整数索引，关联到打开的文件对象。VFS 是内核组织文件操作的公共层，设备节点也通过文件接口参与其中。读写普通文件、管道和设备虽然都能使用 fd，具体语义由资源类型及对应实现决定。

第三，驱动通过 `struct file_operations` 提供 open、read/write、unlocked_ioctl、release 等操作入口。它不需要另一个用户态 main 来持续执行请求。系统调用到达相应操作时，内核在受约束的执行上下文中运行驱动代码；哪些操作会等待、如何同步、谁释放资源，仍要由驱动设计说明。

## 6. 为什么 ioctl 之外还需要协议

针对一个设备，可以比较 `read/write` 字节流接口与 `ioctl` 控制接口。前者适合字节读写，但需要另行约定消息边界、命令及版本；后者可以按请求号分发不同结构，也需要校验结构、长度、指针和权限。ioctl 并不会替驱动创造正确的事务状态机，也不能凭宏定义自动解释所有嵌套用户指针。

因此，系统调用入口和 RPC 协议是两层接口：一次进入内核可以处理零个、一个或多个协议事件，系统调用成功也不等于远端方法已经执行并返回。下一章选择设备接口时，需要先写清这些语义，再决定使用哪些 read/write/ioctl 操作。

## 7. Linux v6.6 Binder 对照

下面只对照固定 [Linux v6.6 驱动基线](../reference/android-binder.md)，不是在本机执行真实 Binder 的验证。2026-11-06 核查了链接的头文件及 `binder_ioctl_write_read`。

| 教学问题与机制 | 源码入口 | 相同点 | 不同点与版本 |
| --- | --- | --- | --- |
| 如何带参数进入内核 | [BINDER_WRITE_READ 定义](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/android/binder.h#L254) | 用户态经 ioctl 进入驱动 | 本章管道无 Binder ioctl；Linux v6.6 |
| 一次入口是否只有一个消息 | [binder_write_read](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/android/binder.h#L176) 与 [binder_ioctl_write_read](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L5049) | 输入/输出缓冲区有长度与已消费字节数 | Binder 可消费多条 BC 命令并返回多项 BR 工作；本实验每次一个字节；Linux v6.6 |
| 如何解释用户态发来的命令 | [binder_thread_write](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L3801) | 需要按协议解释数据并校验 | BC_TRANSACTION/BC_REPLY 是缓冲区里的协议命令，不是各自一次 ioctl；Linux v6.6 |
| 等待结束是否得到业务回复 | [BR_TRANSACTION_COMPLETE](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/android/binder.h#L392)、[BR_REPLY](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/android/binder.h#L374) | 必须解释实际事件类型 | 完成通知与业务回复不同；管道 R/I 握手不等同这些命令；Linux v6.6 |
| 用户指针和失败如何处理 | [binder_ioctl_write_read](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L5049) | 驱动复制外层结构，调用读写处理，并复制消费进度 | 返回错误时不能假设此前所有写命令都没执行；管道实验没有 Binder 事务副作用；Linux v6.6 |

具体地，`binder_write_read` 有 write_size/write_consumed/write_buffer 和对应 read 字段。驱动先按 write_size 处理发送缓冲区，再在需要时读取待办工作。头文件中的 BC/BR 名字虽然也用 `_IO*` 宏编码，它们在这里仍是协议缓冲区中的命令字，不是让应用逐个把 BC_TRANSACTION 当 ioctl 请求号调用。

libbinder 在用户态组织这些命令、解释返回工作并分发方法；驱动不替应用完成普通业务方法或 Parcel 的全部编解码。本章尚未锁定和验证 AOSP 用户态实现，因而不声称已经验证 libbinder 的信号重试策略。

## 8. 验收与练习

运行 `make test`，再按 [实验手册](../labs/syscalls.md) 跟踪两个新程序及已有 Socket RPC。判断依据是事件和调用结果之间的因果关系，不是固定 PID、行号或调度顺序。本次在 Linux 6.8.0-90-generic / glibc 2.35 / strace 5.16 上执行过这些命令；驱动阅读仍是 Linux v6.6，二者不能合并成一个版本基线。

请回答：

1. errno 非零但 write 返回 1 时，能否说写入失败？请用管道读端结果支持判断。
2. 去掉 SIGUSR1 屏蔽，只在 read 前打印就绪，会出现哪一种丢失实验顺序的情况？
3. 为什么父进程要等待 I，才能写入 X？这与随机 sleep 有何不同？
4. 为什么 EINTR 后重试一个就绪等待，并不意味着可以重新提交任意有副作用的 RPC？
5. strace 中出现 clone 和 pselect6，分别能支持什么观察，不能支持哪些进程切换或性能结论？
6. BINDER_WRITE_READ 成功返回、BR_TRANSACTION_COMPLETE、BR_REPLY 分别属于哪一层事件？

本章代码与自动验收已经运行；尚无初学者独立复现记录，不把这一点写成整章发布验收已经完成。
