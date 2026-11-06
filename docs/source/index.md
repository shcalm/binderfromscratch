# 源码阅读入口

以下入口展示本次构建时工作树的源文本。它们不是已发布章节的不可变快照；旧章节的固定提交与重放方式见[代码版本索引](../reference/code-versions.md)。

| 仓库目录 | 内容 |
| --- | --- |
| [examples/](../../examples/) | 普通 C 模型、入门与系统调用实验、最小模块 |
| [driver/](../../driver/) | 当前 mini-binder 教学驱动 |
| [include/](../../include/) | 教学 UAPI |
| [userspace/](../../userspace/) | Server 与 Client 示例 |
| [tests/](../../tests/) | 用户态与 VM 内验收源码 |
| [tools/](../../tools/) | 固定环境构建、QEMU 运行与站点工具 |
| [Makefile](../../Makefile) | 仓库构建及验收命令 |

源码文本会随工作树修改而变化。生成站点后修改代码时，应重新构建以获得新的阅读副本。
