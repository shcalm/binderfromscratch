# 第 09–18 章阅读稿：范围、来源与验证

记录日期：2026-11-10。按用户“把后面的文档先写完，写完后再 review”的安排，本轮补齐十章连续正文，并接入本地阅读站点。当前为 `tutorial-development` 工作树内容，尚未创建新章节发布标签或部署线上站点。

**正文已补齐，mini-binder 实现仍到 C08。** 第 09–15 章是可供评审的后续设计；第 16–18 章是固定版本源码阅读与设计复盘。新章提供的问题推演、普通程序和源码结论各有自己的证据范围，不能合并为新增驱动能力已经验收。

## 1. 从哪里开始 review

[全书阅读顺序](../reading-path.md)提供连续入口；也可以直接从下面的具体问题进入。

| 章节 | 正文解决的问题 | 本轮证据类型 |
| --- | --- | --- |
| [09 服务发现](../chapters/09-service-manager.md) | 晚启动的客户端如何取得第一个引用？ | 多 owner 与 bootstrap 设计、名字表 C 反例、Android14 ServiceManager 阅读。 |
| [10 线程](../chapters/10-thread-model.md) | 等待中的调用者由谁执行同步回调？ | 事件泵、同步栈和退出协议设计，v6.6 调度路径阅读。 |
| [11 FD](../chapters/11-file-descriptors.md) | 文件编号为什么必须经转换与目标安装？ | 可运行 SCM_RIGHTS 实验、持有与交付设计、真实 FD fixup 阅读。 |
| [12 生命周期](../chapters/12-lifetime.md) | 引用还在时服务为什么仍会死亡？ | 强弱状态 C 模型、owner 协作与死亡确认协议设计、版本差异。 |
| [13 安全](../chapters/13-security.md) | 谁提供调用者身份，哪一层决定授权？ | 可运行 SO_PASSCRED 实验、身份与策略设计、真实凭据/LSM 阅读。 |
| [14 Oneway](../chapters/14-oneway.md) | 不等待回复时如何保证有界排队和排序？ | 现有有界队列实验、异步协议设计、v6.6 FREE 推进路径阅读。 |
| [15 并发](../chapters/15-concurrency.md) | 同时调用、取消、释放和退出时谁结账？ | 现有事务 trace、C08 锁审查、分域设计与故障矩阵。 |
| [16 内核架构](../chapters/16-binder-architecture.md) | 从一次调用如何找到 proc/thread/node/ref/alloc？ | v6.6 结构、路由、锁与退出路径的定向源码阅读。 |
| [17 调用追踪](../chapters/17-call-trace.md) | 从 add 参数到结果，九个事件分别发生什么？ | Android14 与 v6.6 的静态调用链，分开跟踪控制、数据、对象和 FD。 |
| [18 设计复盘](../chapters/18-design-review.md) | 哪些简化仍存在，Rust 改变了哪些责任？ | 六组现有反例重放、能力差异矩阵、两类 Rust 实现的固定版本阅读。 |

本轮也保留并接入了 P01 的开头改写：从“让另一个进程计算，原进程怎样取得结果”出发，再安排 fork、等待、线程和退出状态实验。第 08 章结尾、首页、README、路线、实验入口、任务总表与源码基线同步更新。旧 `routes/part-3` 至 `part-6` 地址转为真实章节入口，避免旧链接继续只看到未来提纲。

## 2. 三组版本分别固定

| 来源 | 固定版本 | 用途和限制 |
| --- | --- | --- |
| Linux C Binder | 上游 Linux v6.6 标签 | 全书主要内核阅读与既有 VM 基线；本轮定向补读相关结构、事务、FD、生命周期、安全和异步路径，并非逐行审计整个内核。 |
| AOSP 用户态 | `android-14.0.0_r1` 所指提交 `bfcf75076e562945bae131a4929f29a90d0d2481` | Parcel、IPCThreadState、ProcessState、ServiceManager、代理及 Rust NDK 包装；未构建运行 AOSP。 |
| Linux Rust Binder | 上游 v6.18 所指提交 `7d0a66e4bb9081d75c82ec4957c50034cb0ea449` | 仅第 18 章比较。源码与构建接线证实上游存在可选 Rust 驱动，不证明设备部署、特性相同或性能结论。 |

Linux v6.18 注解标签对象为 `f7b88edb52c8dd01b7e576390d658ae6eef0e134`，本轮通过上游 GitHub 镜像的 tag/ref 元数据核对所指提交；没有执行签名验证。元数据、八份文件和内容哈希保存于本地忽略的 `build/reference/linux-v6.18/`。该补充来源不替换 v6.6 的章节行号或已有 VM 环境。

AOSP 原四文件及其获取工具见[用户态基线](../reference/aosp-userspace.md)；本轮另外按相同提交取下表八份文件，保留原有授权说明。它们是第三方阅读材料，不随本项目 MIT 声明变更授权。额外文件来自 GitHub contents API 的 raw 响应，使用固定 ref；没有把下载到的主线文件冒充固定提交。

## 3. 补充 AOSP 文件的内容校验

缓存根为 `build/reference/aosp-frameworks-native/`。原四文件位于该根目录；本轮额外文件保留以下仓库相对路径。`fetch-aosp.sh` 仍只负责原四文件，并未因为本表增加而自动支持额外下载。

| frameworks/native 内路径 | SHA-256 |
| --- | --- |
| `cmds/servicemanager/ServiceManager.cpp` | `640605b542412faac29bb32ce46888b4d85ca321f5f569addc501aaf237c0cca` |
| `cmds/servicemanager/main.cpp` | `03c6a80b63b025bf206f71321b7625d1ea3642021dffa348381bec27318df502` |
| `libs/binder/Binder.cpp` | `75d776e0af365ded9cb2f47ed0d464818b757ebaf246a2766378dc879bf1a52f` |
| `libs/binder/BpBinder.cpp` | `132d5127c6694353d5a5f655e3a2e001f06e143139c6242da6ca263fcaefdace` |
| `libs/binder/rust/Android.bp` | `6d8a0735cf148fa78c6cd82673b65e19c6d20778bd1c6897b6f212d514daf3d4` |
| `libs/binder/rust/src/lib.rs` | `bbaed015aafbfa0229af3c59905b924bd33268842608572c324808b0ab5dca75` |
| `libs/binder/rust/src/proxy.rs` | `00c0cf89e0de4cc46819bf3694a7a85822d7517e7c9ba960f2084abc62024fdb` |
| `libs/binder/rust/src/parcel.rs` | `5326415a5dbcf65f9b75e077ec81a5f1b5334adfbba2015cd054e3126244f074` |

需要独立获取时，例如下载本章使用的 Rust 代理文件：

```sh
curl --fail --location --max-time 60 \
  --header 'Accept: application/vnd.github.raw+json' \
  --output /tmp/binder-android14-proxy.rs \
  'https://api.github.com/repos/aosp-mirror-neo/platform_frameworks_native/contents/libs/binder/rust/src/proxy.rs?ref=bfcf75076e562945bae131a4929f29a90d0d2481'
sha256sum /tmp/binder-android14-proxy.rs
```

结果必须匹配表中 proxy.rs 的哈希。网络失败、限流返回或哈希不符均不能当作已取得相同源码；换其他表中路径时也要逐一核对内容。阅读完可删除这个 `/tmp` 文件。

## 4. Rust 内核来源的内容校验

下列路径位于上游 Linux v6.18 的 `drivers/android/` 下，缓存保持同样路径。章节只分析涉及的问题与片段，不声称完整审计这些文件的每条分支。

| drivers/android 内路径 | SHA-256 |
| --- | --- |
| `Kconfig` | `6a456777334ffccbe8fc646121494c068e4950482cb5c145062b892733ed9e68` |
| `Makefile` | `372d0be7d7552f8979568b93e1c0e761761c10ac952b8a4885e6216860154771` |
| `binder/Makefile` | `bb10b7877a67416109acc6b9a1f093f49623c938c239af4b031a58018d70dda9` |
| `binder/rust_binder_main.rs` | `b6e394915daabcc0c536c8be631e795bc3ed90b2a4b91953d9ccb837edc4b2b9` |
| `binder/process.rs` | `15cd45852b68a3644b2a13960b08d94517fd8bf3632607b314159148b2739d3b` |
| `binder/thread.rs` | `83a6932e6e6dd84b88424d70c4ca4e89e2e4a4b209a590d4efbd7e8b300bc97f` |
| `binder/transaction.rs` | `03c60bbcf31eded09b0e61ba1c8ee94dca40f52add3acf0233c2556176a1e6c3` |
| `binder/node.rs` | `e160f676df24a64861bc100dce8c1235c9316d6892296f1193c92c87557db9a1` |

获取方式相同，仓库换成 torvalds/linux，路径和 ref 使用本表及固定提交。例如：

```sh
curl --fail --location --max-time 60 \
  --header 'Accept: application/vnd.github.raw+json' \
  --output /tmp/binder-linux-v618-transaction.rs \
  'https://api.github.com/repos/torvalds/linux/contents/drivers/android/binder/transaction.rs?ref=7d0a66e4bb9081d75c82ec4957c50034cb0ea449'
sha256sum /tmp/binder-linux-v618-transaction.rs
```

该文件按原 SPDX 标识保留授权；本轮未将八份缓存加入项目发布内容。复核时还应检查 Kconfig 与 Makefile，避免从一份单独 `.rs` 文件推断合入和构建状态。

## 5. 本轮已经运行的普通实验

写作和审读中实际执行了以下实验。新例子的完整源码就在正文；它们用于隔离问题，没有接入 mini-binder。

| 位置 | 实际观察 | 结果与限制 |
| --- | --- | --- |
| 第 09 章 C 名字表 | 旧实例删除迟到，不删除同名新实例 | 编译及运行退出 0；不模拟驱动、凭据或进程间发现。 |
| 第 11 章 Python SCM_RIGHTS | 相同 FD 数字可指向不同文件；转交后读出 AB/CD/EF，发送者 close 不销毁接收者持有 | 运行退出 0；Socket 文件传递，不是 mini FD 测试。 |
| 第 12 章 C 强弱状态 | 最后 strong 消失后弱持有保留身份，禁止复活，最后 weak 归还后可回收控制块 | 编译及运行退出 0；无并发原子协议或 owner 协作。 |
| 第 13 章 SO_PASSCRED | 正文声称 UID 0，接收方取得发送者真实 UID 1000 | 完整 shell 实验退出 0；使用真实 UID/GID 的 Socket 凭据，不等同于教学有效 UID 策略。 |
| 第 14 章 request_queue fifo | 两项容量满后第三项拒绝，出队有序 | 运行退出 0；没有 one-way Node 调度。 |
| 第 15 章 transactions trace | 回复与取消各自先到时只有一方终结；复制故障各自结账 | 三条正文 trace 均退出 0；普通状态模型，不是故障注入驱动。 |
| 第 18 章六组已有实验 | fork 隔离、方法/对象身份、raw/transfer 引用、乱序事务 | 全部退出 0；最后活资源计数回到 0 的模型已检查。 |

第 18 章重放原始日志和命令摘要位于本地 `artifacts/remaining-chapters/`，摘要为 `replays.json`。它们是本轮本地证据；不是已经对外发布的实验包，也没有代替早期 VM 报告。

## 6. 审读与站点验证

独立审读覆盖第 09–12、16–18 章的关键语义与来源。审读发现的 FD 输出 EFAULT 重试冲突、同步 submit 栈前置条件、发布者身份来源和 add 去重优先级已在对应章修正；同步 COMPLETE 与库内部封装返回的表述也作了澄清。所有新章再统一核查版本边界、可执行命令、站内目标与前后衔接。

文档检查与 git diff --check 均退出 0；make test 的 101 项用户态测试全部通过。MkDocs 严格构建通过，静态检查覆盖 146 个生成页面，站内页面、锚点及资源目标错误为 0。补充来源的 16 份文件哈希也已逐项与上表核对。

浏览器检查实际覆盖 45 条路由各两种尺寸，共 90 个页面，包含全部新章、报告和四个旧路线入口。Mermaid、公式、搜索、手机菜单均通过，无脚本异常、整页横向溢出或外部资源请求。新章桌面与手机截图已打开检查；宽时序图在图内横向滚动以保留可读字号，最后一章的账目复核图调整为窄屏可完整显示的纵向图，并另行复核。路由和显示检查不等于教学内容已通过读者验收。

可重复的检查命令：

```sh
python3 tools/check-docs.py
git diff --check
make test
.venv/docs/bin/python -m mkdocs build --strict --clean
.venv/docs/bin/python tools/site_check.py site
SITE_PREVIEW_URL=http://127.0.0.1:8001 \
SITE_BROWSER_EXECUTABLE=/home/airdrop/.cache/ms-playwright/chromium-1228/chrome-linux64/chrome \
  node tools/site_browser_check.mjs
```

本次浏览器使用已生成 site/ 的 8001 静态服务，先按[站点说明](../site-guide.md)在另一终端启动该服务；浏览器可执行文件是本机缓存路径，其他环境安装或指定自己的 Chromium。截图和 results.json 保存在忽略的 `artifacts/site-browser/`。供日常阅读的 MkDocs 预览地址为 <http://127.0.0.1:8000/binderfromscratch/>，P01 和第 09–18 章均从当前工作树构建。

## 7. 保持开放的实现与验收

本轮没有改变 mini-binder ABI 或驱动，没有新增管理器、嵌套线程模型、FD、完整强弱协议、死亡通知、授权策略、oneway 或锁分域实现；没有重新运行 VM，没有构建 AOSP/Rust 内核，没有部署 GitHub Pages，也没有执行完整无障碍、打印或外链在线验证。

任务总表中含“实现、实验、教学验收”组合条件的项目继续未勾选，计数仍为 93/216。C08 开发标签保持原样。后续按用户 review 修改正文，再分别安排实现闭环、真实系统重放与章节正式发布；正文齐全不自动跨过这些门槛。
