# 映射的地址、权限与生命周期

对应 C04-02。源码是 [mapping.c](../../examples/memory/mapping.c)，自动验收是 [test_mapping.py](../../tests/test_mapping.py)。这是普通 Linux 用户态实验，不加载 mini-binder、不打开 Binder 设备、不需要 root。本实验自身不修改驱动；真正的映射 RPC 是[第 04 章](../chapters/04-mmap.md)中的另一项增量实现，两者不能混作同一条数据路径。

## 从已有 RPC 推导映射方案

已有 RPC 把参数送进驱动，再把结果交给接收者。假设未来接收的是较大且需要反复查看的字节区：能否让接收者直接通过自己的虚拟地址读到一个受管理的接收区？先要回答三个问题：谁能修改内容，何时可以读取，关闭入口 fd 后谁继续拥有内容。

两种合理方案都能传递数据。通过管道或 Socket 发送字节，接收者读入自己的缓冲，能独立修改和保留该副本，协议和所有权通常比较直接；共享映射让双方使用各自地址访问同一后备对象，省去应用在每次查看时再次调用 read 的需要，但必须另行定义同步、访问权限和复用时机。映射本身不提供“消息已经完整”“对方已经读完”或“该缓冲现在可以覆盖”的语义。选择共享映射，是为了隔离这些责任，不是在此证明它对全部负载性能更优。

本实验的决策是：用 `memfd_create` 创建匿名内存文件，`ftruncate` 将长度设为一页，双方对文件偏移 0 做 `MAP_SHARED`；父进程映射 `PROT_READ | PROT_WRITE`，子进程单独建立 `PROT_READ` 映射。fork 会先继承父的 RW 映射，子进程显式解除这份继承映射，再建立 RO 映射，避免只读实验仍暗留 RW 别名。memfd 的 fd 最初是可读写的；这里展示的是当前映射权限，不是防恶意进程的完整权限隔离。这个 RO 映射仍可能通过 `mprotect` 升级权限，实验没有封印 memfd，也没有模拟 Binder 对 `VM_MAYWRITE` 的限制。

两个单向管道仅传递控制 token 和读取确认。父进程收到 ready 后才开始写入；每次写完发 R，子进程收到 R 才读取并回复实际值；父进程收到确认后才覆盖下一轮。管道也复制其少量控制消息和确认值，不能据此把整个程序称为“零复制”。`volatile` 用于保留实验中的实际访问，排序依靠 Linux 管道握手协议，不依靠 volatile、随机 sleep 或运行耗时。

## 运行与验收

前置条件是支持 `memfd_create` 的 Linux、C 编译器和 Python 3。受限制的沙箱若禁止 memfd、mmap、fork 或 `prctl`，应报告为运行失败，不能跳过后算通过。在仓库根目录运行：

```sh
mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic examples/memory/mapping.c -o build/mapping
./build/mapping
./build/mapping --abort-after-map
python3 -m unittest discover -s tests -p test_mapping.py -v
```

正常运行返回 0；主动中止模式预期返回 1；未知选项返回 2，stderr 含 `usage:`，不创建实验资源。不要在 `set -e` 脚本里把主动中止的退出 1 当成非预期失败。五项自动验收必须全部通过，它会自行在临时目录用相同的警告选项编译，结束时清理，不依赖 Makefile 或 `build/mapping`。

一次正常运行的事件顺序如下，地址和页大小由运行环境决定，不应写死；两个进程的数值地址允许相同：

```text
event=mapped bytes=4096 parent_address=0x... child_address=0x... parent_prot=rw child_prot=r guards=none
event=fd_closed parent=1 child=1
event=published round=1 first=7 last=70
event=read_ack round=1 first=7 last=70
event=published round=2 first=42 last=420
event=read_ack round=2 first=42 last=420
event=fault kind=ro_write signal=11
event=preserved first=42 last=420
event=fault kind=guard_read signal=11
event=cleanup children_reaped=2 unmapped=1 result=ok
```

`guards=none` 表示两侧保护页的权限是 `PROT_NONE`，并非没有保护页。自动验收使用系统的 `SIGSEGV` 常量，不假定所有 Linux 架构信号编号都是 11。程序不从子进程直接打印数据，而由父进程在收到实际确认或 waitpid 状态后打印事件，避免多进程 stdout 交错让记录失真。

## 观察一：虚拟地址与共享内容

假设：父进程更新相同后备对象的偏移后，子进程可以在自己的映射中读到更新。第一轮写入第一个 `uint32_t` 为 7、最后一个为 70；确认之后第二轮改为 42、420。两组 `published → read_ack` 顺序和数值都必须匹配；不匹配、管道失败或子进程异常退出都使程序返回 1。

映射地址是进程虚拟地址；相同数字在不同进程里不代表任意内容天然共享，不同数字也不代表后备数据不同。本例共享的是 memfd 同一文件偏移的数据。Linux 的共享文件映射机制允许页表指向同一后备页，但本实验不读取 PFN，不证明物理地址相同、页常驻、物理连续或实际分配页数。页可能按需建立或被换出；`mmap` 成功不等于整段物理内存已预先分配。本实验也没有建立 MAP_PRIVATE 对照来观察写时复制。

VMA 是内核描述一段虚拟地址范围、权限和映射关系的记录，不等于物理页，也不等于消息。本例先预留三页匿名 `PROT_NONE` 地址范围，再用 `MAP_FIXED` 仅替换自己已预留的中间一页为 memfd 映射，留下两侧保护页。这样的替换不覆盖其他已有用途的映射；任意地址直接使用 MAP_FIXED 则可能破坏进程地址空间。首尾数据访问检验的是中间数据页内的有效范围，不是物理页布局。

## 观察二：close 与映射生命周期

假设：`close(fd)` 只撤销这个 fd 引用，已有映射仍然有效。子进程建立 RO 映射、关闭自己的 memfd fd，才发送 ready；父进程收到 ready 后关闭自己的 fd。`fd_closed parent=1 child=1` 必须发生在两轮发布和确认之前，因此两轮读取都发生在双方关闭 fd 之后。实验中不再有 memfd fd，也不重新打开文件。

结论是该映射持有后备对象的必要引用，fd 的生命周期和映射的生命周期不同。父进程最后用 `munmap` 解除整段预留范围，子进程终止时由内核解除它的映射；最后一个相关引用消失后，memfd 对象才可回收。日志不能证明机器上每个物理页即时回到空闲列表，也不能把 memfd 的回收规则原样套给任意字符设备驱动。

## 观察三：权限与范围失败

假设：同一后备数据可在不同映射中拥有不同的当前访问权限。父进程在收到两轮确认后发送 W；RO 子进程对首元素执行一次 volatile 写入。父进程必须用 `waitpid` 取得 `WIFSIGNALED` 且 `WTERMSIG == SIGSEGV` 的状态，才打印 `kind=ro_write`。如果子进程正常退出、收到别的信号或等不到退出，均为失败。随后父进程重读首尾元素，必须仍是 42、420，才打印 `preserved`。这证明这次非法写未修改观测到的数据，不证明 RO 读取者获得了不可变快照：父进程仍可继续修改共享页。

第二个受控子进程继承父进程的 RW 数据页及保护页，读取数据页结束地址的一个 `uint32_t`；该地址是后侧 `PROT_NONE` 保护页的起点，因此预期同样由 `SIGSEGV` 结束。父进程检查并回收后打印 `kind=guard_read`。这是有意受控的 Linux 非法访问实验；不要在业务代码中以 C 越界访问作为检查手段。

任意 mmap 末尾之后不一定没有别的有效 VMA，故不能靠随手 `ptr[length]` 得到的“没崩溃”推断映射范围。保护页让这里的故障原因明确。本例文件和数据映射都恰为整页，未测试非整页长度、文件截短造成的 `SIGBUS`、`mprotect`、拆分 VMA 或重复解除映射，也不宣称所有映射错误都产生 `SIGSEGV`。

## 截止时间、失败与清理

程序以单调时钟设定从启动起五秒的共同截止时间，所有管道 I/O 通过 poll 使用剩余时间；等待退出用 `waitpid(WNOHANG)` 和最长 10ms 的 poll 复查。这个复查间隔仅用于有界观察子进程退出，数据读写顺序由控制 token 决定。系统调用失败、超时和 SIGINT/SIGTERM 均进入父进程清理路径。该截止时间约束正常调度下的协议等待，并非对进程被 SIGSTOP、内核不可中断等待或系统停顿的硬实时保证。

`--abort-after-map` 在 ready 和关闭 fd 后主动失败，此时 RO 子进程仍在等第一个 R。stderr 必须出现 `requested abort after map`，不得出现 `read_ack`，最后应为：

```text
event=cleanup children_reaped=1 unmapped=1 result=failed
```

失败清理会关闭所有已建立的 fd、SIGKILL 尚未回收的子进程并 waitpid，然后解除父进程映射。正常路径恰好回收两个故障子进程；中止路径回收一个等待子进程。预期故障不会终止父进程。程序启动时设 `RLIMIT_CORE=0` 并设 `PR_SET_DUMPABLE=0`，后者也防止启用管道 core handler 的宿主为故障探针生成 dump。内核日志或外部监控仍可能记录信号事件。

测试进程另有八秒外层 timeout，编译有三十秒 timeout；超时不是通过。不模拟 SIGKILL 掉父进程后的完整资源审计；进程映射和 fd 有内核退出回收，但这不能替代一般服务的取消设计。运行结束无命名共享内存文件、无持久后台子进程；手动编译生成的 `build/mapping` 可用 `rm -f build/mapping` 清理。

## 对照 Linux v6.6 Binder

以下只对照本问题相关入口，核查对象是仓库构建树 `build/kernel/linux-6.6/drivers/android/` 中的 `binder.c` 与 `binder_alloc.c`，版本固定为上游 Linux v6.6。链接按该版本文件和函数定位，不把浮动主线行号当证据。

| 教学问题 | 本实验机制 | v6.6 入口及相同点 | 不同点 |
| --- | --- | --- | --- |
| 谁能建立映射、谁能写 | memfd 两份 MAP_SHARED，父 RW、子 RO | [binder_mmap](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c) 检查打开者线程组、拒绝 `VM_WRITE`，清除 `VM_MAYWRITE`，设置 `VM_DONTCOPY`、`VM_MIXEDMAP`；权限是显式设计 | Binder 接收区不能按本例 fork 继承模型使用，也不是发送者可写、接收者只读的跨进程共享 memfd |
| 映射范围何时成为数据页 | 一页文件映射，缺页由普通文件映射机制处理 | [binder_alloc_mmap_handler、binder_update_page_range](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder_alloc.c) 分别初始化接收区管理结构，以及按需要 `alloc_page`、`vm_insert_page` | handler 的分配器范围最多 4MiB；建立范围和为事务提供物理页是不同步骤，本例不模拟 Binder 缓冲分配树 |
| 数据怎样进入接收区 | 父直接写共享后备对象；子只读 | [binder_alloc_copy_user_to_buffer](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder_alloc.c) 获取目标缓冲的页、`kmap_local_page` 后调用 `copy_from_user` | Binder 仍将发送者普通数据复制进接收者的 Binder 缓冲；本例直接共享发送端写入区不是 Binder 实际实现，也不能从时间推断复制次数 |
| 解除映射意味着什么 | munmap 或退出解除映射，最后引用释放 memfd | [binder_vma_close](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c) 调用 [binder_alloc_vma_close](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder_alloc.c) 将分配器 VMA 清空，阻止新的入站缓冲分配 | Binder 的 VMA 关闭、事务完成、缓冲协议释放和分配器最终清理是不同事件；不能把一个 close 或 munmap 当成全部责任已经结束 |

本例的两端能观察相同数据，只证明这里建立的共享和同步关系有效。它没有 Binder transaction、`BR_TRANSACTION` 数据指针、`BC_FREE_BUFFER`、对象/FD 转换或内核并发分配，也没有测吞吐。真实 Binder 收到事务后通过接收区地址访问内容，不等于应用没有 Parcel 编解码或额外复制。接下来的问题是：在接收区内如何验证长度与偏移、分配和复用多个缓冲，并保证读完前不被覆盖；这些需要另行设计协议和生命周期。
