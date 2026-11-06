# P02 系统调用、errno 与中断实验

对应 P02-02、P02-03、P02-04；概念与设计比较见 [P02 正文](../chapters/p02-syscalls.md)。本实验只运行普通用户态进程，不加载模块，不需要 root。

## 1. 环境、源码与验收

源码：[errno_write.c](../../examples/syscalls/errno_write.c)、[interrupt_wait.c](../../examples/syscalls/interrupt_wait.c)。自动验收：[test_syscalls.py](../../tests/test_syscalls.py)。

```sh
make
make test
```

前置条件：Linux、C11/POSIX 编译环境、Python 3；跟踪实验另需 strace 和允许跟踪自身子进程的环境。本次 2026-11-06 实测为 Linux 6.8.0-90-generic、glibc 2.35、strace 5.16。该宿主环境不同于 Linux v6.6 Binder 阅读基线，也不同于后续 VM 驱动环境。

每项程序正常实验返回 0，非法策略参数返回 2，内部断言或系统错误返回 1。Python 自动测试对单次程序执行设 5 秒截止；程序自身没有 RPC 截止时间。所有父进程正常路径均关闭不用的端点并 waitpid 指定子进程，不创建后台任务。

## 2. 成功返回与旧 errno

假设：成功调用不要求清除旧 errno；不能单看 errno 是否非零判断失败。

```sh
./build/errno_write
```

本机输出：

```text
event=write_success return=1 errno=33 decision=success
event=payload_read return=1 value=X
event=write_failure return=-1 errno=9 decision=stop
```

观察证据：write 返回 1，管道另一端读到了 X，而 errno 仍是预设 EDOM。对照负例使用非法 fd=-1，返回 -1 且 errno=EBADF。保存 errno 的动作紧接系统接口返回，先于打印和 close。

失败判据：成功写入未送达 X、非法 fd 被当作成功、或者程序非零退出。测试对本次 Linux/glibc 包装器保留 EDOM 的现象做回归；不能据此要求所有平台上所有成功库函数都保持 errno。

清理：程序关闭两个管道端点后退出，无子进程。

## 3. 显式握手、信号中断与两种策略

假设：信号确实中断一次等待后，应用可以按接口与业务语义选择重试或停止。

```sh
./build/interrupt_wait --retry
./build/interrupt_wait --stop
```

retry 预期三个事件：`waiting`、`interrupted ... errno=4 signal_seen=1 action=retry`、`read_complete ... value=X interruptions=1`。stop 预期前两个事件后出现 `cancelled interruptions=1`，没有 `read_complete`。

观察依赖两个控制字节，不能用它们替换成延时：R 表示子进程已建立屏蔽信号的等待阶段；I 表示子进程已观察 pselect 返回 EINTR。父进程收到 R 后才发 SIGUSR1，收到 I 后才可能写业务字节 X。

SIGUSR1 在 pselect 之外被屏蔽；pselect 用临时掩码原子地解除屏蔽并进入等待。信号先于系统调用抵达也会保留为待处理信号，数据管道在 I 之前始终为空，因此不会因“数据抢先到达”而错过 EINTR。信号处理函数只设置 sig_atomic_t 标志，打印与握手发生在普通执行流中。

这里动态验证的是 pselect 等待被中断，不声称已经动态验证阻塞 read、所有 ioctl 或自动重启行为。read 在等待可读之后才执行，且只有这一个读者。

失败判据：第一次等待未返回 EINTR、signal_seen 不是 1、retry 未读到 X、stop 仍读取 X、事件顺序错误、非零退出或超过外层 5 秒截止。

清理：父进程关闭数据写端与控制读端；子进程关闭其余端点并退出；父进程 waitpid 回收。数据端 EOF 也保证父进程退出并关闭写端时，子进程不会继续永久等待该端点。SIGPIPE 被忽略以便让管道写失败返回错误，正常路径不触发它。

结论边界：测试选择“EINTR 后允许重试等待”和“EINTR 后停止”两种策略；它没有实现重发去重、RPC 取消或部分业务回滚。不能把等待重试推广为无条件重试任意失败调用。

## 4. 实际跟踪现有 RPC

将日志放入忽略的 artifacts，避免把本次 PID 或机器地址写入受版本控制的源码：

```sh
mkdir -p artifacts/syscalls
strace -f -o artifacts/syscalls/rpc.trace -e trace=socketpair,clone,sendto,recvfrom,close,wait4 ./build/socket_rpc 1 2
```

本次已执行，程序返回 0、result=3。检查日志中的因果链：

- Socket pair 创建成功，fork 对应的 clone 产生另一 PID。
- 客户端 sendto 发送 12 字节，服务端 recvfrom 收到该请求。
- 服务端 sendto 发送 12 字节回复，客户端 recvfrom 收到它。
- 各自关闭端点，父进程 wait4 回收对应子进程。

动态加载器也可能先 read/close 文件；更完整的无过滤跟踪会看到这类活动，不属于 RPC 请求。`<unfinished ...>` 与 `<... resumed>` 应按 PID 和调用对应起来；两个 PID 的输出可以交错，不能断言固定文本行序。本机 libc 选择 clone，别的平台可能使用不同入口；如果过滤结果缺少进程创建事件，先去掉过滤再核查。

失败判据：程序非零退出、请求或回复未完成、没有父子两端证据。strace 受到 ptrace 权限限制而无法启动时，是观察工具不可用，不能写成 RPC 失败或成功。清理：被跟踪程序正常结束后 strace 自行退出，日志文件保留供检查。

## 5. 跟踪中断与 errno

```sh
strace -f -o artifacts/syscalls/interrupt.trace -e trace=pipe,pipe2,clone,read,write,pselect6,rt_sigprocmask,rt_sigaction,kill,wait4 ./build/interrupt_wait --retry
strace -o artifacts/syscalls/errno.trace -e trace=read,write,close ./build/errno_write
```

两条命令本次都返回 0。interrupt.trace 中已观察到：SIGUSR1 被屏蔽；子进程写 R；父进程 kill(SIGUSR1)；pselect6 发生信号中断；子进程写 I；父进程写 X；下一次 pselect6 返回可读，read 返回 X；wait4 回收子进程。

本次 strace 在信号前显示 `ERESTARTNOHAND (To be restarted if no handler)`，然后显示 SIGUSR1；这是内核/跟踪层的重启标记，不是应用应从 errno 中读取的错误码。程序输出确认 libc 接口实际返回 -1、errno=EINTR。不能因为 trace 中存在 restart 字样，就说应用没有收到 EINTR。

errno.trace 中可核查向管道 write 返回 1、对 -1 描述符 write 返回 `-1 EBADF`。errno 的线程本地存储赋值属于用户态行为，strace 不会自动显示全部 errno 变化；“成功后仍是 EDOM”来自程序立即捕获值与读端数据的组合证据。

失败判据：程序事件和 trace 无法对应、把内部重启码当成用户 errno、只看到系统调用就推断切换了进程、或由跟踪耗时给出性能结论。strace 会扰动调度，本实验不做性能测量。

清理日志可运行：

```sh
rm -f artifacts/syscalls/rpc.trace artifacts/syscalls/interrupt.trace artifacts/syscalls/errno.trace
```

## 6. Android 对照与未覆盖项

已对照 [Linux v6.6 binder_write_read](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/android/binder.h#L176) 的长度、缓冲区地址和 consumed 字段，以及 [binder_ioctl_write_read](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L5049) 的复制和先写后读路径。一次 BINDER_WRITE_READ 可以承载多条 BC/BR 协议工作；ioctl 返回与业务回复仍需分开解释，不能用本章一个字节的管道模拟推导完整 Binder 行为。

没有动态测试真实 Binder、libbinder 重试、部分传输、信号风暴、跨架构包装器差异或驱动 copy_*_user 失败。没有初学者独立复现记录。完整教学对照表与练习在正文，后续设备章节应继续定义其自己的失败和资源语义。
