# 用户态进程与 RPC 边界实验

对应任务：P01-03、P01-04、C01-04、C01-05。源码位于 [examples/intro](../../examples/intro/)，验收位于 [test_process.py](../../tests/test_process.py) 和 [test_intro.py](../../tests/test_intro.py)。实验只运行普通用户态程序，不需要 root、内核模块或 Android 设备。

## 1. 准备与统一验收

在仓库根目录运行：

```sh
make
make test
```

需要支持 C11/POSIX 的 C 编译器、pthread 和 Python 3。程序使用 Linux 的 `AF_UNIX/SOCK_SEQPACKET` 与 `MSG_TRUNC` 语义。编译选项包含 `-Wall -Wextra -Werror -pedantic`；线程程序额外使用 `-pthread`。

验收必须全部为 `ok`，`make test` 返回 0。下面返回 2、3 的故障注入命令是预期失败，不应直接放进遇错即退出的 shell 串联命令。

## 2. 线程共享不是数据竞争

问题：子进程改了 `result`，父进程仍看到 -1；换成同一进程中的线程会怎样？

```sh
./build/fork_private
./build/thread_shared
```

第一个程序预期 `child result=3`、`parent result=-1`。第二个程序预期以下三行，地址和 PID 按本次运行变化，但三行内部相同：

```text
event=before_create result=-1 address=<same-address> pid=<same-pid>
event=worker_write result=3 address=<same-address> pid=<same-pid>
event=after_join result=3 address=<same-address> pid=<same-pid>
```

主线程先打印初值，再创建工作线程；工作线程通过参数中的指针修改同一个 `int`；主线程在 `pthread_join()` 成功后才读取。这些读写由创建和 join 建立顺序，不依赖 `volatile` 或碰巧调度，没有通过数据竞争证明共享。

比较另外两个方案：让主线程 `sleep()` 不能建立 C 内存模型中的同步关系；多个线程持续访问共享变量时可以使用互斥锁，而本例只需等待一次计算完成，join 足够。线程共享地址空间不表示可以任意并发读写。指针值也只在相应地址空间中有意义，不能据此证明两个进程访问同一个对象。

失败判据：事件顺序错误、join 后仍为 -1、三行地址或 PID 不同，或者非零退出。清理：程序正常 join 后退出，无后台线程。

## 3. 退出状态不能代替普通函数返回值

```sh
./build/fork_status --exit
./build/fork_status --signal
```

正常退出模式预期：

```text
child_result=300
event=exited exit_status=44 parent_result=-1
```

子进程调用 `_exit(300)`；POSIX 退出状态只保留低 8 位，父进程通过 `WIFEXITED` 后的 `WEXITSTATUS` 得到 44。这与能传输完整 32 位业务结果的请求/回复协议不同。父进程自身的 `result` 仍是 -1。

信号模式在子进程内恢复并解除屏蔽 `SIGTERM`，然后 `raise(SIGTERM)`，不使用非法内存访问、不生成 core dump。Linux 上预期：

```text
event=signaled signal=15 parent_result=-1
```

父进程先判断 `WIFSIGNALED`，再读取 `WTERMSIG`；不能在信号退出分支使用 `WEXITSTATUS` 猜业务结果。两种模式的外层实验程序都返回 0，表示观察到指定现象，并非声称子进程都正常退出。

`fork()` 失败时返回 -1，没有可等待的子进程；不能把 -1 当 PID 调用 `waitpid(-1, ...)`，因为后者表示等待任意子进程。

为了动态验证失败，不必耗尽宿主资源。运行 `./build/fork_failure`，它只把自己的 RLIMIT_NPROC 软限制设为 0，再尝试一次 fork，预期 `event=fork_rejected errno=11 child_created=0`（Linux EAGAIN），外层实验返回 0。限制属于这个短暂进程，不会修改启动它的 shell 或其他程序的限制。普通用户可直接运行；以 root 运行时示例先只在自身进程中降 UID 到 65534，因为特权进程可以绕过此限制。若容器不允许该降权或仍保有绕过限制的能力，则明确报错，不把 fork 成功误认为实验通过。该实验只验证一种资源限制拒绝，不穷尽 fork 的所有失败原因。

失败判据：44 被误报为 300、信号退出被归为正常退出、父变量变化，或者实验返回非零。清理：父进程 `waitpid()` 回收指定子进程；仅在 `EINTR` 时重试。

## 4. 长度错误与未知操作是不同失败

```sh
./build/socket_rpc 1 2 --short-request
./build/socket_rpc 1 2 --long-request
./build/socket_rpc 1 2 --unknown-op
```

协议正常请求是三个网络字节序的 `uint32_t`，共 12 字节。短包只发送 8 字节；超长包发送 16 字节。接收时仍只提供 12 字节缓冲区，但 Linux `MSG_TRUNC` 返回原始包长，因此不能把超长包的前 12 字节误当成合法请求。

两个长度错误模式都预期 stderr 包含 `server protocol error: malformed request` 和 `transport error: no complete reply`，退出码 3，没有 `result=`。服务端不解释不完整的字段，不回复业务结果，关闭端点后客户端得到失败。

未知操作模式发送长度正确但操作码为 99 的包。预期 `remote error: unknown operation`、退出码 2，没有 `transport error`。这说明结构合法与业务操作合法是两层检查：一个协议能承载明确的远端错误，不必把所有失败混成连接断开。

失败判据：非法长度获得结果，长包被截断后接受，未知操作被当作 add，或者客户端永久等待。清理：每次客户端关闭端点并等待服务端退出。测试外层另设 5 秒截止，避免实现回归挂住整套验收；该截止不属于 RPC 协议。

## 5. 服务端活着，不代表调用一定完成

不要直接裸跑无回复模式并等待它自行结束。用有清理逻辑的验收入口：

```sh
python3 -m unittest discover -s tests -p test_intro.py -k live_server_without_reply -v
```

实验的精确顺序：

1. 客户端发送正常 12 字节请求。
2. 服务端完整接收后，通过独立控制 Socket 发送就绪字节，然后等待控制端点关闭，故意不发 RPC 回复。
3. 父进程收到就绪字节才打印 `event=request_received_no_reply server_pid=...` 并 flush。此时“已收到请求”有显式握手证据。
4. 客户端通过没有截止参数的 `pselect()` 等待 RPC 端点可读；测试确认子进程存在，并等待 0.2 秒，预期 Python 抛出 `TimeoutExpired`。超时后父子进程仍存在。
5. 测试给父进程发送 `SIGTERM`。信号处理函数只设置 `sig_atomic_t` 标志。`pselect()` 原子地解除 SIGTERM 屏蔽并等待，避免检查标志与进入等待之间丢失终止通知。
6. 父进程打印 `harness interruption: no protocol timeout`，关闭 RPC 和控制端点，服务端从控制端点读到 EOF 后退出；父进程 `waitpid()` 回收它，返回 124。测试确认子 PID 已不存在。

测试自身等待就绪和清理各有 5 秒上限；异常失败时会终止独立进程组。0.2 秒只是测试观察窗口，不是任意 sleep 制造执行顺序，也不承诺某台机器的调度性能。

这里有三个不同事件：RPC 端点收到 EOF 表示对端关闭；Python 的截止表示测试不再等待；124 表示本实验按测试信号完成清理。协议没有实现每次请求的超时、取消消息、远端停止执行或迟到回复处理。控制 Socket 只用于本实验的同步和清理，不是对外 RPC 功能。一次无回复观察只证明“连接存活不会自动产生业务回复”，不能证明无限未来永不回复。

失败判据：没有收到就绪事件就报告实验成功、服务端提前退出、客户端自行产生结果、测试截止被写成协议超时，或清理后仍有子进程。

## 6. Android 对照的边界

驱动阅读基线仍是 [Linux v6.6](../reference/android-binder.md)，本实验没有运行 Android Binder。概念对应关系如下：

| 本实验观察 | 阅读 Binder 时追问 | 不能推断 |
| --- | --- | --- |
| join 后同一进程线程看到共享值 | 工作由哪个线程领取，数据与对象属于哪个上下文 | `binder_proc` 简单等同 PID，或线程共享不需同步 |
| 子进程退出有专用状态 | 进程退出与在途事务失败如何传播 | waitpid 退出码等于 Binder 业务返回值 |
| 完整包与合法操作分别校验 | `binder_thread_write` 如何消费命令并检查用户数据 | Socket 的 12 字节协议兼容 BC/BR 命令 |
| 请求已收到仍没有业务回复 | `BR_TRANSACTION_COMPLETE` 与 `BR_REPLY` 为什么不同 | 本实验控制握手就是 Binder 完成通知 |
| 测试截止后由外部清理 | 驱动、用户态库及业务各自承担哪些等待和取消责任 | 某个 Binder 版本必然有或没有通用 RPC 超时 |

这些实验用于提出需求和避免错误类比；真实 Binder 的具体失败、等待和生命周期语义需要继续按固定源码版本追踪并在对应环境验证。
