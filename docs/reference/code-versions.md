# 章节代码与版本索引

当前处于开发阶段，以下提交是可检查的代码快照，不等于已完成全章发布。正式发布标签将固定且不移动；没有通过教学与实验门槛的章节不能因存在 Git 提交而算完成。

| 内容 | 代码入口 | 版本与环境 | 实验入口与限制 |
| --- | --- | --- | --- |
| P01 进程/线程 | [入门实验](../../examples/intro/) | 当前主线；Linux/POSIX | `make test`，普通用户态，无真实 Binder 调用 |
| P02 系统调用 | [系统调用实验](../../examples/syscalls/) | 当前主线；实际 trace 环境见实验说明 | `make test`、[实验手册](../labs/syscalls.md) |
| P03/P04 模块/VM | [模块](../../examples/modules/hello/)、[环境](../../tools/kernel/manifest.md) | Linux 6.6；旧查询环境 BusyBox 1.30.1，新环境见阶段报告 | VM 加载/卸载、主动回滚；没有预构建下载发布 |
| 第 01 章 RPC 对照 | [socket_rpc.c](../../examples/intro/socket_rpc.c) | 当前主线，自有 12 字节实验协议 | 正常、错误报文、无回复观察；不兼容 Binder ABI |
| 第 02 章查询设备 | [ABI 决策](../design/device-abi.md) | `6406d8d42e45358a9b289d080376ad858d4c2465`；Linux 6.6 | 此提交是纯查询设备，无 CALL；后续命令不能用于该快照 |
| 第 03 章最小 RPC | [设计](../design/minimal-rpc.md)、[驱动](../../driver/mini_binder.c)、[用户态](../../userspace/) | `cfd7f42`；版本 1 ABI 新增命令 | 八客户端、容量64、中断与退出；尚无 mmap/Node/Ref |
| 第 04 章接收映射 | [设计](../design/receive-region.md)、[验收](../reports/m2-mapping-progress.md) | `dev-c04-20261106` 开发快照；Linux 6.6，ABI 1 增量命令 | 只读映射、16 槽、显式 FREE、退出和复制失败回滚；尚无 Parcel/Node/Ref；不是正式章节发布 |
| 第 05 章 Parcel 请求 | [设计](../design/parcel-format.md)、[验收](../reports/m2-parcel-progress.md)、[用户态库](../../userspace/parcel.h) | `dev-c05-20261106` 开发快照；Linux 6.6，ABI 1 增量命令 | MBP1、映射解码、对象表边界；所有对象拒绝；回复仍是整数，不兼容 libbinder |
| 第 06–18 章 | [任务列表](../plans/task-backlog.md) | 尚未实现/验收 | 路线和任务不是可运行代码版本 |

查询版本重放时在独立目录 checkout 指定提交，按该提交自己的脚本构建。主线源码和旧模块混用不构成正确的版本对照。正式阶段发布时本表补充不可移动标签、完整环境清单和 diff 链接。

第 02 → 03 章的关键增量可直接查看：

```sh
git diff 6406d8d cfd7f42 -- driver/mini_binder.c include/uapi/mini_binder.h
git show cfd7f42:tests/kernel/rpc_test.c
git show cfd7f42:userspace/add_server.c
git diff cfd7f42 dev-c04-20261106 -- driver/mini_binder.c include/uapi/mini_binder.h tests/kernel/mapping_test.c
git diff dev-c04-20261106 dev-c05-20261106 -- driver/ include/uapi/ userspace/ tests/kernel/parcel_test.c
```

`dev-c04-20261106`、`dev-c05-20261106` 是不可移动的开发检查点，后续修改创建新快照，不在原标签上覆盖。它们固定可运行代码和当时文档，但不补足独立初学者复现、预构建产物发布或正式章节验收。按标签独立 checkout 后执行 `make test`、P04 构建和 VM 命令；不要将当前工作树模块用于旧快照。

实际结果及历史日志校验见[阶段报告](../reports/m0-m1-progress.md)和[映射报告](../reports/m2-mapping-progress.md)。
