# 章节代码与版本索引

当前处于开发阶段，以下提交是可检查的代码快照，不等于已完成全章发布。正式发布标签将固定且不移动；没有通过教学与实验门槛的章节不能因存在 Git 提交而算完成。

| 内容 | 代码入口 | 版本与环境 | 实验入口与限制 |
| --- | --- | --- | --- |
| P01 进程/线程 | [入门实验](../../examples/intro/) | 当前主线；Linux/POSIX | `make test`，普通用户态，无真实 Binder 调用 |
| P02 系统调用 | [系统调用实验](../../examples/syscalls/) | 当前主线；实际 trace 环境见实验说明 | `make test`、[实验手册](../labs/syscalls.md) |
| P03/P04 模块/VM | [模块](../../examples/modules/hello/)、[环境](../../tools/kernel/manifest.md) | Linux 6.6；旧查询环境 BusyBox 1.30.1，新环境见阶段报告 | VM 加载/卸载、主动回滚；没有预构建下载发布 |
| 第 01 章 RPC 对照 | [socket_rpc.c](../../examples/intro/socket_rpc.c) | 当前主线，自有 12 字节实验协议 | 正常、错误报文、无回复观察；不兼容 Binder ABI |
| 第 02 章查询设备 | [ABI 决策](../design/device-abi.md) | `6406d8d42e45358a9b289d080376ad858d4c2465`；Linux 6.6 | 此提交是纯查询设备，无 CALL；后续命令不能用于该快照 |
| 第 03 章最小 RPC | [设计](../design/minimal-rpc.md)、[驱动](../../driver/mini_binder.c)、[用户态](../../userspace/) | 当前开发主线；版本 1 ABI 新增命令 | 八客户端、容量64、中断与退出；尚无 mmap/Node/Ref |
| 第 04–18 章 | [任务列表](../plans/task-backlog.md) | 尚未实现/验收 | 路线和任务不是可运行代码版本 |

查询版本重放时在独立目录 checkout 指定提交，按该提交自己的脚本构建。主线源码和旧模块混用不构成正确的版本对照。正式阶段发布时本表补充不可移动标签、完整环境清单和 diff 链接。

实际结果及历史日志校验见[阶段报告](../reports/m0-m1-progress.md)。
