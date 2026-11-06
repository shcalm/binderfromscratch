# 本地阅读站点：构建与验证

本地站点使用 MkDocs Material，正文仍以 `docs/` 中 Markdown 为单一来源。当前是开发阅读版，不代表 M1 或 M2 正式发布。现有正文明确标注初稿，第 06–18 章只进入路线与任务。

## 安装与预览

在仓库根目录运行，使用 Python 3.10 或更新版本：

```sh
python3 -m venv .venv/docs
.venv/docs/bin/python -m pip install --require-hashes -r requirements-docs.txt
.venv/docs/bin/python -m mkdocs serve --dev-addr 127.0.0.1:8000
```

打开 <http://127.0.0.1:8000/>；端口已占用时将命令中的端口改为 8001。无需 sudo。若宿主 Python 缺少 ensurepip / venv 支持，已有 uv 时可直接复用其虚拟环境安装能力：

```sh
uv venv --allow-existing --python /usr/bin/python3 .venv/docs
uv pip sync --python .venv/docs/bin/python requirements-docs.txt
.venv/docs/bin/python -m mkdocs serve --dev-addr 127.0.0.1:8000
```

## 严格构建与链接检查

```sh
.venv/docs/bin/python -m mkdocs build --strict --clean
.venv/docs/bin/python tools/site_check.py site
python3 tools/check-docs.py
```

`site/` 是可部署的静态产物，已经被 Git 忽略；`.venv/` 和 `artifacts/` 也已忽略。链接检查解析实际生成的 HTML，核对站内页面、目录入口、锚点、脚本、样式及搜索索引，不将构建成功等同于正文教学验收。外部 URL 单独计数，不声称网络有效性已检查；Android 对照保留版本与限制说明。

依赖入口为 `requirements-docs.in`，`requirements-docs.txt` 固定包括传递依赖在内的版本及下载哈希。更新依赖时重新生成锁定文件并重跑构建和检查：

```sh
uv pip compile requirements-docs.in --generate-hashes --output-file requirements-docs.txt
```

## 源码与章节版本

`tools/site_hooks.py` 在构建时生成 `source/` 阅读页与目录页；这些页面只进入静态产物，不写回 `docs/`。正文、实验及报告中指向仓库源码的相对链接，通过 Markdown 解析树转到对应生成页。缺失的源码不会被伪造为阅读页，会由严格构建或静态链接检查报告。

源码页有明确的仓库路径和工作树版本说明，展示本次构建时的源文本，不代表第 02 章固定提交。章节标签、环境和重放方式仍以[代码版本索引](reference/code-versions.md)为准。[实验入口](labs/index.md)提供现有命令、预期证据和本地 diff 方法；尚无正式发布标签。

## 静态预览与部署边界

构建后可脱离 MkDocs 用普通 HTTP 服务检查产物：

```sh
.venv/docs/bin/python -m http.server 8001 --bind 127.0.0.1 --directory site
```

打开 <http://127.0.0.1:8001/>。实际托管时将整个 `site/` 上传到支持目录 `index.html` 的静态服务器；子路径托管须保留全部相对资源和目录结构，再执行链接与浏览器检查。当前没有配置虚构的远程仓库、域名或自动发布流程，也没有执行对外部署。

## 本次验证记录

可选浏览器检查独立安装 Playwright，不参与站点运行依赖。先启动上述预览服务，再在另一个终端运行：

```sh
npm install --prefix artifacts/site-browser --no-audit --no-fund playwright@1.56.1
npx --prefix artifacts/site-browser playwright install chromium
node tools/site_browser_check.mjs
```

已有 Chromium 时可设置 `SITE_BROWSER_EXECUTABLE` 为其可执行文件，`SITE_PREVIEW_URL` 可指定不同预览地址。检查覆盖 1365×900 和 390×844 下的已有章节、源码、实验入口与路线，实际等待 Mermaid SVG、公式 SVG 和搜索结果，检查整页横向溢出；截图和结果写入忽略的 `artifacts/site-browser/`。

Mermaid 11.4.1 与 MathJax 3.2.2 的浏览器包及许可证保存在 `docs/assets/vendor/`，通过本地静态资源渲染；`manifest.json` 记录来源和 SHA-256，静态检查会核对其完整性。模板在 Material 初始化前加载固定 Mermaid 版本，避免主题回退到在线下载。字体使用系统字体；站点阅读不依赖字体 CDN。无脚本时保留图表和公式的源文本，核心论证仍在 Markdown 正文中。

记录日期：2026-11-06。实际环境为 Python 3.10.12、MkDocs 1.6.1、Material 9.6.20；系统缺少 ensurepip，实际用 uv 将锁定依赖装入 `.venv/docs`。浏览器为 Playwright 1.56.1 驱动的 Chrome for Testing 149.0.7827.55。

本次严格构建、站点内部页面/锚点/静态资源/供应资源哈希检查、仓库 Markdown 目标检查均退出 0。浏览器检查覆盖含第 04 章及映射报告在内的 28 个桌面/窄屏页面；Mermaid SVG、公式 SVG、搜索结果与手机菜单均实际验证，没有整页横向溢出或脚本异常。测试阻断全部外部资源，未检测到外部资源请求。截图已检查，原始截图与 `results.json` 留在 `artifacts/site-browser/`，不纳入发布内容。

本次使用已有 Chromium 缓存，实际浏览器命令为：

```sh
SITE_BROWSER_EXECUTABLE=/home/airdrop/.cache/ms-playwright/chromium-1228/chrome-linux64/chrome node tools/site_browser_check.mjs
```

同日第 05 章增量验证：加入 Parcel 正文、模型、报告和共用库源码页面后，严格构建及内部链接/资源检查通过（112 页、13608 个站内链接与资源目标、0 错误），浏览器扩大至 36 个桌面/窄屏页面并通过。第 05 章两种尺寸的截图已检查；仍未执行下面列出的完整无障碍、打印和外链在线验证。

这不代表完整无障碍审计、所有手机/打印布局验收或外部 Android 源码 URL 的在线有效性验证；后续正文变化后仍应重跑相同检查。未执行对外部署，也未宣布 M1/M2 正式发布。

公式渲染检查样例：\(a + b = c\)。无脚本时保留公式源文本。
