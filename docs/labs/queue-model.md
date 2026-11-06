# 从单槽到请求队列：用户态模型

对应 C03-02。源码是 [request_queue.c](../../examples/models/request_queue.c)，验收是 [test_queue_model.py](../../tests/test_queue_model.py)。这是一个确定顺序执行的普通 C 程序，不打开设备，不创建线程，不需要 root。

## 运行与验收

在仓库根目录运行：

```sh
mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic examples/models/request_queue.c -o build/request_queue
./build/request_queue overwrite
./build/request_queue refuse
./build/request_queue fifo
python3 -m unittest discover -s tests -p test_queue_model.py -v
```

五项测试必须全部通过。测试自行在临时目录编译模型，结束时清理，不依赖 `build/request_queue` 或 Makefile 是否列出它。未知模式返回 2，三个有效模式返回 0。

## 一个槽必须作出选择

两名 Client 分别提交请求 101（Client 1，1 + 2）和 102（Client 2，19 + 23）。两次提交都发生在服务端领取之前。

`overwrite` 模式直接让第二笔请求替换槽中的第一笔。输出包含 `event=overwritten request_id=101 client=1`，最后只有 Client 2 得到 42。程序明确记录第一笔丢失，而不是把它误称为已完成。实际同步 RPC 若丢失这笔请求且没有失败通知，Client 1 就可能一直等下去。

`refuse` 模式保留请求 101，拒绝请求 102，输出 `reason=slot_full`。处理第一笔后，模型显式执行 `event=retry request_id=102`，两名 Client 才分别得到 3 和 42。拒绝本身不是数据丢失，但调用方必须决定是否重试。另一种选择是让第二名 Client 阻塞等待空槽；这需要定义等待条件、唤醒、中断与退出，不能仅靠一个共享变量完成。

单槽加拒绝也可以是正确协议。这里选择队列，是为了在有界资源内同时保留多笔已接受请求，并观察请求与结果的对应关系，不是证明队列对所有负载都更好。

## FIFO 和回复关联解决不同问题

`fifo` 模式的等待队列容量是 2，先接收 101、102。第三笔 103 被拒绝，输出 `reason=queue_full`，没有获得结果。

随后有两组不同的顺序：

```text
event=delivered request_id=101
event=delivered request_id=102
event=client_result client=2 request_id=102 result=42
event=client_result client=1 request_id=101 result=3
```

FIFO 约束的是领取顺序；模型故意反过来回复，证明回复需要按请求 ID 找到对应记录，不能简单地把“下一份结果”交给“下一名等待者”。当前独立 add_server 是串行领取并回复的；反序回复是模型中的受控情景，不代表该服务端已经实现并行执行。

回复前还必须检查状态。模型验证三个反例：ID 999 不存在，输出 `reason=unknown`；101 尚未领取，输出 `reason=not_delivered`；102 已回复，再次回复输出 `reason=duplicate`。这些被拒绝的回复不产生 `client_result`。这也解释为什么一笔记录除了 a/b，还需要标识、归属和状态。

队列移除已领取记录，只表示它不再等待领取，不能马上销毁记录。模型的全部请求记录由 main 的数组一直保留到退出，`reply()` 才能找到已交付请求。在真实驱动里，这需要明确的引用和释放责任，见 [同步 RPC 设计](../design/minimal-rpc.md)。

## 与设备实验的边界

模型按固定顺序调用函数，没有互斥锁、等待队列、信号、内核用户复制或真实进程退出。它不能证明真实并发中没有丢失唤醒、竞态或内存泄漏，也没有模拟取消与迟到回复的竞争。输入是固定且不会溢出的数值；数值解析和业务溢出由独立设备演示处理。

模型的容量 2 仅限制等待领取的指针数；已领取记录仍在数组中。驱动的 `MB_MAX_REQUESTS` 则限制尚未最终回收的整笔请求资源，不能把两个计数直接比较。模型没有请求动态分配，结束时由普通进程退出回收其用户态内存。

设备演示源码为 [add_server.c](../../userspace/add_server.c) 和 [add_client.c](../../userspace/add_client.c)，需由 VM 构建入口放入镜像，在 VM 内运行。Server 使用 `add_server COUNT`，COUNT 范围为 1 到 1000000，注册成功后输出并 flush `READY`；Client 使用 `add_client U32 U32`。正常输出 `result=...`，业务溢出输出 `business error:` 并返回 2，设备或 ioctl 失败输出 `transport error:` 并返回 3。Server 每领取并成功回复一笔就计数，达到 COUNT 后关闭设备退出；不要配置超过预计调用数量后再期待它自行结束。

两个演示在 SIGINT 或 SIGTERM 下使用只调用 `_exit()` 的信号处理函数，以 128 加信号编号退出；内核关闭设备 fd 并执行相应回收。它们没有自动重试 CALL，也不承诺被中断时远端业务尚未执行。服务端回复已取消请求得到的 ENOENT 作为通信失败报告并退出；长期服务的恢复策略不在此有限演示范围内。
