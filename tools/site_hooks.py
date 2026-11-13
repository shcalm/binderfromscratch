"""Build source reading pages without copying or editing tutorial Markdown."""
from pathlib import Path
import hashlib
import posixpath
import re
from urllib.parse import unquote, urlsplit, urlunsplit

from markdown.extensions import Extension
from markdown.treeprocessors import Treeprocessor
from mkdocs.plugins import event_priority
from mkdocs.structure.files import File

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
SOURCE_ROOTS = ("driver", "include", "userspace", "examples", "tests", "tools")
SOURCE_NAMES = {"Makefile", "Kbuild", "config", "init", "manifest.md"}
SOURCE_SUFFIXES = {".c", ".h", ".py", ".sh", ".mjs", ".html"}
source_pages = {}
current_page = None


def source_uri(path):
    relative = path.relative_to(ROOT).as_posix()
    return "source/" + relative + ("/index.md" if path.is_dir() else ".md")


class SourceLinks(Treeprocessor):
    def run(self, root):
        if current_page is None or (current_page.file.src_uri.startswith("source/")
                                    and current_page.file.src_uri != "source/index.md"):
            return
        origin = DOCS / current_page.file.src_uri
        for node in root.iter("a"):
            href = node.get("href", "")
            parts = urlsplit(href)
            if parts.scheme or parts.netloc or not parts.path:
                continue
            target = (origin.parent / unquote(parts.path)).resolve()
            if target not in source_pages:
                continue
            relative = posixpath.relpath(
                source_pages[target], posixpath.dirname(current_page.file.src_uri)
            )
            node.set("href", urlunsplit(("", "", relative, parts.query, parts.fragment)))


class SourceLinksExtension(Extension):
    def extendMarkdown(self, md):
        # Run before MkDocs' relative-link resolver (priority 0).
        md.treeprocessors.register(SourceLinks(md), "repository_source_links", 5)


def on_config(config):
    config.markdown_extensions.append(SourceLinksExtension())
    return config


def on_files(files, config):
    source_pages.clear()
    parts = (
        (3, "第三篇：对象与服务", (
            ("07 Node 对象身份", "07-nodes"),
            ("08 Ref 与对象转交", "08-references"),
            ("09 ServiceManager 服务发现", "09-service-manager"))),
        (4, "第四篇：线程与生命周期", (
            ("10 线程与嵌套回调", "10-thread-model"),
            ("11 文件描述符传递", "11-file-descriptors"),
            ("12 引用生命周期与死亡通知", "12-lifetime"))),
        (5, "第五篇：安全与稳定性", (
            ("13 身份与授权", "13-security"),
            ("14 Oneway 与背压", "14-oneway"),
            ("15 并发与资源回收", "15-concurrency"))),
        (6, "第六篇：真实 Binder", (
            ("16 从需求定位内核架构", "16-binder-architecture"),
            ("17 逐事件追踪一次调用", "17-call-trace"),
            ("18 设计复盘与 Rust 对照", "18-design-review"))),
    )
    for number, title, chapters in parts:
        content = f"# {title}：阅读入口\n\n"
        content += "本篇正文已可阅读。第 09–18 章为设计与源码阅读稿；"
        content += "对应 mini-binder 扩展仍须实现和实测。\n\n"
        for label, stem in chapters:
            content += f"- [{label}](../chapters/{stem}.md)\n"
        content += f"\n[全书阅读顺序](../reading-path.md#part-{number})与"
        content += "[任务总表](../plans/task-backlog.md)分别记录正文和实现进度。\n"
        files.append(File.generated(config, f"routes/part-{number}.md", content=content))
    sources = {ROOT / "Makefile"}
    for directory in SOURCE_ROOTS:
        for path in (ROOT / directory).rglob("*"):
            if path.is_file() and not path.is_symlink() and (
                path.suffix in SOURCE_SUFFIXES or path.name in SOURCE_NAMES
            ):
                sources.add(path)
    directories = set()
    for path in sources:
        parent = path.parent
        while parent != ROOT:
            directories.add(parent)
            parent = parent.parent
    for path in sources | directories:
        source_pages[path.resolve()] = source_uri(path)
    notice = (
        "> 本页为构建时工作树的源代码阅读副本，不是章节发布快照。"
        "版本及旧章节重放方式见[代码版本索引]({version})。\n\n"
    )
    for path in sorted(sources):
        uri = source_pages[path.resolve()]
        relative = path.relative_to(ROOT).as_posix()
        language = {".c": "c", ".h": "c", ".py": "python", ".sh": "sh",
                    ".mjs": "javascript", ".html": "html"}.get(
            path.suffix, "makefile" if path.name in {"Makefile", "Kbuild"} else "text"
        )
        content = path.read_text(encoding="utf-8")
        fence = "`" * max(3, max((len(part) for part in re.findall(r"`+", content)), default=0) + 1)
        version = posixpath.relpath("reference/code-versions.md", posixpath.dirname(uri))
        page = f"# 源代码：{relative}\n\n" + notice.format(version=version)
        digest = hashlib.sha256(content.encode("utf-8")).hexdigest()
        page += f"仓库路径：`{relative}`。源文本 SHA-256：`{digest}`。\n\n"
        page += f"{fence}{language}\n{content.rstrip()}\n{fence}\n"
        files.append(File.generated(config, uri, content=page))
    for directory in sorted(directories):
        uri = source_pages[directory.resolve()]
        label = directory.relative_to(ROOT).as_posix() + "/"
        page = f"# 源代码目录：{label}\n\n"
        page += "这是构建时工作树的源码索引；仅收录教学代码、测试与构建脚本。\n\n"
        for child in sorted(sources | directories):
            if child.parent == directory:
                target = posixpath.relpath(source_pages[child.resolve()], posixpath.dirname(uri))
                page += f"- [{child.name}{'/' if child.is_dir() else ''}]({target})\n"
        files.append(File.generated(config, uri, content=page))
    return files


@event_priority(100)
def on_pre_page(page, config, files):
    global current_page
    current_page = page
    return page


def on_page_markdown(markdown, page, config, files):
    if page.file.src_uri.startswith("chapters/"):
        lines = markdown.splitlines(keepends=True)
        chapter = Path(page.file.src_uri).stem.split("-", 1)[0]
        if chapter.isdigit() and int(chapter) >= 9:
            notice = (
                "\n> **章节状态：设计与源码阅读稿，待 review。** "
                "当前 mini-binder 实现到第 08 章；本章分别标明设计推演、"
                "可运行示例和源码阅读的证据范围。\n"
            )
        else:
            notice = "\n> **章节状态：开发初稿，尚未正式发布。** 实验与验收边界见本章和阶段报告。\n"
        lines.insert(1, notice)
        return "".join(lines)
    return markdown
