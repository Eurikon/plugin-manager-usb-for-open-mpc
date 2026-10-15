from __future__ import annotations

import argparse
import hashlib
import html
import re
import shutil
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from urllib.parse import quote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
DOCUMENTS = {
    "README.md": ("guide.html", "User & build guide"),
    "DEVELOPER_FAQ.md": ("developer.html", "Developer FAQ"),
    "CHANGELOG.md": ("changelog.html", "Changelog"),
}
SIDEBAR_LABELS = {
    "using-a-prepared-usb-bundle": "USB Setup",
    "find-update-and-remove-plugins": "Manage Plugins",
    "what-changes-on-the-mpc": "MPC Changes",
    "building-the-manager": "Build Manager",
    "building-a-usb-bundle-without-redistributing-firmware": "Build USB",
    "broader-offline-tests": "Offline Tests",
    "source-layout-and-integration": "Source Layout",
    "credits-and-acknowledgments": "Credits",
    "licensing": "License",
    "disclaimer": "Disclaimer",
    "building-and-finding-the-code": "Build Guide",
    "firmware-launch-and-session-lifetime": "Launch Session",
    "packages-settings-and-saved-data": "Package Data",
    "testing-and-troubleshooting": "Testing Help",
    "contributing-and-preparing-releases": "Contribute",
}
ASSETS = {
    "styles.css": "assets/styles.css", "site.js": "assets/site.js", "assets/manager-catalog.png": "assets/manager-catalog.png",
    **{"assets/tab-{}.svg".format(kind): "assets/tab-{}.svg".format(kind) for kind in ("catalog", "local", "installed", "history")},
}
NOTICES = {"LICENSE": "license.txt", "licenses/community-tools.txt": "community-tools-license.txt"}


def repository_name(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*/[A-Za-z0-9][A-Za-z0-9_.-]*", value):
        raise argparse.ArgumentTypeError("repository must be OWNER/REPO")
    return value


def revision_name(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", value):
        raise argparse.ArgumentTypeError("revision must be a branch name or commit without slashes")
    return value


def slug(text: str) -> str:
    text = re.sub(r"\[([^]]+)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"[^\w\s-]", "", text.lower()).strip()
    return re.sub(r"\s", "-", text)


def sentence_breaks(fragment: str) -> str:
    tokens: list[str] = []

    def protect(match: re.Match) -> str:
        value = match[0]
        kind = "C" if value.startswith("</") else "T"
        tokens.append(value)
        return "\x00{}{}\x00".format(kind, len(tokens) - 1)

    fragment = re.sub(r"<(code|pre)\b[^>]*>.*?</\1>", protect, fragment, flags=re.S)
    fragment = re.sub(r"<[^>]+>", protect, fragment)
    boundary = re.compile(r"([.!?](?:[\"'”’)]|&quot;|&#x27;)*)((?:\x00C\d+\x00)*)[ \t]+(?=\S)")

    def split(match: re.Match) -> str:
        prefix = fragment[:match.start(1) + 1]
        if re.search(r"\b(?:e\.g|i\.e|etc|vs|Dr|Mr|Mrs|Ms|Prof|No)\.$", prefix, re.I):
            return match[0]
        return match[1] + (match[2] or "") + "<br>\n"

    fragment = boundary.sub(split, fragment)
    return re.sub(r"\x00[CT](\d+)\x00", lambda m: tokens[int(m[1])], fragment)


def manager_icon(kind: str) -> str:
    shapes = {
        "catalog": ([(col * 13, row * 13, 9, 9) for row in range(2) for col in range(2)], []),
        "local": ([(0, 5, 24, 2), (0, 0, 11, 2), (0, 0, 2, 22), (22, 5, 2, 17), (0, 20, 24, 2)], [(10, 0, 15, 5)]),
        "installed": ([], [(0, 12, 8, 20), (8, 20, 23, 2)]),
        "history": ([(3, 2, 21, 2), (22, 2, 2, 21), (3, 21, 21, 2), (3, 12, 2, 10)], [(13, 6, 13, 13), (13, 13, 18, 15), (2, 9, 7, 4), (2, 9, 0, 3)]),
    }
    rectangles, lines = shapes[kind]
    for x, y, end_x, end_y in lines:
        dx, sx = abs(end_x - x), 1 if x < end_x else -1
        dy, sy = -abs(end_y - y), 1 if y < end_y else -1
        error = dx + dy
        while True:
            rectangles.append((x, y, 2, 2))
            if x == end_x and y == end_y:
                break
            twice = 2 * error
            if twice >= dy:
                error += dy
                x += sx
            if twice <= dx:
                error += dx
                y += sy
    path = " ".join("M{} {}h{}v{}h-{}z".format(x, y, width, height, width) for x, y, width, height in rectangles)
    return '<svg class="view-icon" viewBox="0 0 25 24" width="25" height="24" aria-hidden="true" focusable="false" fill="currentColor" shape-rendering="crispEdges"><path d="{}"/></svg>'.format(path)


@dataclass
class Heading:
    level: int
    title: str
    anchor: str


class Markdown:
    def __init__(self, root: Path, repository: str = "", revision: str = "main") -> None:
        self.root = root
        self.repository = repository
        self.revision = revision
        self.headings: list[Heading] = []
        self.used: dict[str, int] = {}

    def link(self, label: str, destination: str) -> str:
        parts = urlsplit(destination)
        if parts.scheme:
            if parts.scheme not in ("https", "http", "mailto"):
                return '<span class="source-reference">' + label + "</span>"
            href = destination
        elif parts.netloc or destination.startswith("/"):
            return '<span class="source-reference">' + label + "</span>"
        elif not parts.path:
            href = destination
        else:
            path = PurePosixPath(parts.path)
            if ".." in path.parts:
                return '<span class="source-reference">' + label + "</span>"
            name = str(path)
            if name in DOCUMENTS:
                href = DOCUMENTS[name][0]
            elif name in NOTICES:
                href = NOTICES[name]
            elif self.repository and (self.root / name).is_file():
                href = "https://github.com/{}/blob/{}/{}".format(
                    self.repository, quote(self.revision, safe=""), quote(name, safe="/")
                )
            else:
                return '<span class="source-reference" title="Source file in the repository">' + label + "</span>"
            if parts.fragment:
                href += "#" + parts.fragment
        return '<a href="{}">{}</a>'.format(html.escape(href, quote=True), label)

    def inline(self, value: str, tokens: list[str] | None = None) -> str:
        if tokens is None:
            tokens = []

        def token(rendered: str) -> str:
            tokens.append(rendered)
            return "\x00{}\x00".format(len(tokens) - 1)

        def image(match: re.Match) -> str:
            parts = urlsplit(match[2])
            if parts.scheme == "https" and parts.netloc == "img.shields.io":
                return token('<img class="readme-badge" src="{}" alt="{}" height="20" loading="lazy">'.format(
                    html.escape(match[2], quote=True), html.escape(match[1], quote=True)
                ))
            if match[2] not in ASSETS:
                destination = match[2].removeprefix("site/")
                source = destination if destination in ASSETS else ""
            else:
                source = match[2]
            if source and source.endswith(".svg"):
                return token('<img class="tab-icon" src="{}" alt="{}" width="20" height="20" loading="lazy">'.format(
                    ASSETS[source], html.escape(match[1], quote=True)
                ))
            if not source or not source.endswith(".png"):
                return token(html.escape(match[1]))
            return token('<img src="{}" alt="{}" width="1280" height="800" loading="lazy">'.format(
                ASSETS[source], html.escape(match[1], quote=True)
            ))

        value = re.sub(r"!\[([^]\n]*)\]\(([^)\n]+)\)", image, value)
        value = re.sub(
            r"\[([^]\n]+)\]\(([^)\n]+)\)",
            lambda m: token(self.link(self.inline(m[1], tokens), m[2])),
            value,
        )
        value = re.sub(r"`([^`]+)`", lambda m: token("<code>" + html.escape(m[1]) + "</code>"), value)
        value = re.sub(r"<br\s*/?>", lambda m: token("<br>"), value, flags=re.I)
        value = html.escape(value)
        value = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", value)
        value = re.sub(r"(?<!\*)\*([^*]+)\*(?!\*)", r"<em>\1</em>", value)
        return re.sub(r"\x00(\d+)\x00", lambda m: tokens[int(m[1])], value)

    def heading(self, level: int, text: str) -> str:
        base = slug(text)
        count = self.used.get(base, 0)
        self.used[base] = count + 1
        anchor = base if count == 0 else "{}-{}".format(base, count)
        self.headings.append(Heading(level, text, anchor))
        return '<h{0} id="{1}">{2}<a class="heading-link" href="#{1}" aria-label="Link to this section">#</a></h{0}>'.format(
            level, html.escape(anchor, quote=True), self.inline(text)
        )

    @staticmethod
    def list_item(line: str):
        return re.match(r"^(\s*)([-+*]|\d+\.)\s+(.*)$", line)

    def render_list(self, lines: list[str], index: int, indent: int) -> tuple[str, int]:
        first = self.list_item(lines[index])
        ordered = first[2][0].isdigit()
        tag = "ol" if ordered else "ul"
        output = ["<{}>".format(tag)]
        while index < len(lines):
            match = self.list_item(lines[index])
            if not match or len(match[1]) != indent or match[2][0].isdigit() != ordered:
                break
            output.append("<li>" + self.inline(match[3]))
            index += 1
            while index < len(lines):
                line = lines[index]
                nested = self.list_item(line)
                if nested:
                    if len(nested[1]) > indent:
                        fragment, index = self.render_list(lines, index, len(nested[1]))
                        output.append(fragment)
                        continue
                    break
                if not line.strip():
                    next_index = index + 1
                    while next_index < len(lines) and not lines[next_index].strip():
                        next_index += 1
                    following = self.list_item(lines[next_index]) if next_index < len(lines) else None
                    if following and len(following[1]) >= indent:
                        index = next_index
                        if len(following[1]) == indent:
                            break
                        continue
                    break
                leading = len(line) - len(line.lstrip())
                if leading <= indent:
                    break
                output.append(" " + self.inline(line.strip()))
                index += 1
            output.append("</li>")
        output.append("</{}>".format(tag))
        return "\n".join(output), index

    def render(self, text: str, paragraph_breaks: bool = True) -> str:
        lines = text.splitlines()
        output: list[str] = []
        index = 0
        while index < len(lines):
            line = lines[index]
            if not line.strip():
                index += 1
                continue
            fence = re.match(r"^```([A-Za-z0-9_-]*)\s*$", line)
            if fence:
                code = []
                index += 1
                while index < len(lines) and not lines[index].startswith("```"):
                    code.append(lines[index])
                    index += 1
                output.append('<pre><code{}>{}</code></pre>'.format(
                    ' class="language-{}"'.format(fence[1]) if fence[1] else "",
                    html.escape("\n".join(code) + "\n"),
                ))
                index += 1
                continue
            heading = re.match(r"^(#{1,6})\s+(.+)$", line)
            if heading:
                output.append(self.heading(len(heading[1]), heading[2]))
                index += 1
                continue
            if line.startswith("|") and index + 1 < len(lines) and re.match(r"^\|[\s:|-]+\|\s*$", lines[index + 1]):
                cells = lambda row: [self.inline(cell.strip()) for cell in row.strip().strip("|").split("|")]
                output.append('<div class="table-scroll" tabindex="0" aria-label="Scrollable table"><table><thead><tr>')
                output.extend("<th scope=\"col\">{}</th>".format(cell) for cell in cells(line))
                output.append("</tr></thead><tbody>")
                index += 2
                while index < len(lines) and lines[index].startswith("|"):
                    output.append("<tr>" + "".join("<td>{}</td>".format(cell) for cell in cells(lines[index])) + "</tr>")
                    index += 1
                output.append("</tbody></table></div>")
                continue
            item = self.list_item(line)
            if item:
                rendered, index = self.render_list(lines, index, len(item[1]))
                output.append(rendered)
                continue
            paragraph = [line.strip()]
            index += 1
            while index < len(lines) and lines[index].strip():
                if re.match(r"^(#{1,6})\s|^```|^\|", lines[index]) or self.list_item(lines[index]):
                    break
                paragraph.append(lines[index].strip())
                index += 1
            content = self.inline(" ".join(paragraph))
            output.append("<p>" + (sentence_breaks(content) if paragraph_breaks else content) + "</p>")
        return "\n".join(output)


def section(text: str, title: str) -> str:
    match = re.search(r"^## " + re.escape(title) + r"\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    if not match:
        raise ValueError("missing README section: " + title)
    return match[1].strip()


def render_faq(renderer: Markdown, text: str) -> str:
    output: list[str] = []
    group = False
    for chunk in re.split(r"(?=^## |^### )", text, flags=re.M):
        if chunk.startswith("## "):
            if group:
                output.append("</section>")
            output.append('<section class="faq-group">' + renderer.render(chunk))
            group = True
        elif chunk.startswith("### "):
            output.append('<article class="faq-entry" data-faq>' + renderer.render(chunk) + "</article>")
        else:
            output.append(renderer.render(chunk))
    if group:
        output.append("</section>")
    return "\n".join(output)


def shell(title: str, description: str, current: str, body: str, repository: str, style_version: str) -> str:
    document_title = title if current == "index.html" else title + " · Plugin Manager USB"
    links = (("index.html", "Overview"), ("guide.html", "Guide"), ("developer.html", "Developer FAQ"), ("changelog.html", "Changelog"))
    nav = "".join('<a href="{}"{}>{}</a>'.format(path, ' aria-current="page"' if path == current else "", label) for path, label in links)
    source = '<a href="https://github.com/{}">Source repository</a>'.format(repository) if repository else "<span>Source repository link not configured</span>"
    return '''<!doctype html>
<html lang="en" data-theme="dark">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="dark light">
  <meta name="description" content="{description}">
  <title>{title}</title>
  <link rel="icon" href="data:,">
  <link rel="stylesheet" href="assets/styles.css?v={style_version}">
  <script src="assets/site.js" defer></script>
</head>
<body>
<a class="skip-link" href="#content">Skip to content</a>
<header class="site-header">
  <div class="header-inner">
    <a class="brand" href="index.html" aria-label="Plugin Manager USB overview"><span class="title-pill"><span>PLUGIN MANAGER</span><b>USB</b></span></a>
    <nav aria-label="Main navigation">{nav}</nav>
    <button class="theme-toggle" type="button" aria-label="Switch to light theme" aria-pressed="false"><span aria-hidden="true">◐</span><span class="theme-label">Light</span></button>
  </div>
</header>
<main id="content" tabindex="-1">{body}</main>
<footer class="site-footer"><div><a href="guide.html#credits-and-acknowledgments">About this project</a></div><div>{source}</div></footer>
</body>
</html>
'''.format(title=html.escape(document_title), description=html.escape(description, quote=True), nav=nav, body=body, source=source, style_version=style_version)


def landing(readme: str, root: Path, repository: str, revision: str) -> str:
    renderer = Markdown(root, repository, revision)
    compatibility = re.search(r"\*\*(MPC Live .*? / AKAI MPC OS .*?)\*\*", readme)
    if not compatibility:
        raise ValueError("README must state tested hardware compatibility")
    host_section = section(readme, "Building the Manager")
    host_match = re.search(r"(Development tools .*?)(?=\n\nRequirements:)", host_section, re.S)
    if not host_match:
        raise ValueError("README must describe the validated development environment")
    host = host_match[1]
    session_paragraphs = section(readme, "Using a Prepared USB Bundle").split("\n\n")
    session_notice = next((paragraph for paragraph in session_paragraphs if paragraph.startswith("**Save your project first.**")), "")
    if not session_notice:
        raise ValueError("README must explain saving the project and the MPC application restart")
    if repository:
        download = "https://github.com/{}/releases/download/v1.0.3/plugin-manager-usb-v1.0.3.zip".format(repository)
        actions = '<a class="button primary" href="{}">Download USB installer <span aria-hidden="true">↓</span></a><a class="text-link" href="guide.html#using-a-prepared-usb-bundle">USB setup guide <span aria-hidden="true">→</span></a>'.format(download)
        release = '<a href="https://github.com/{}/releases/tag/v1.0.3">Version 1.0.3 · release details and SHA-256 checksum</a>'.format(repository)
        preparation = "Download the USB installer."
    else:
        actions = '<a class="button primary" href="guide.html#using-a-prepared-usb-bundle">Read the USB guide <span aria-hidden="true">↗</span></a><a class="text-link" href="developer.html">Explore the source <span aria-hidden="true">→</span></a>'
        release = "Local preview: no installer download link configured."
        preparation = "Obtain a prepared USB installer."
    return '''
<section class="hero wrap">
  <div class="hero-copy"><h1>Plugin Manager USB for OPEN MPC</h1><p class="hero-intro">{intro}</p><div class="hero-actions">{actions}</div><p class="release-state"><span class="status-dot" aria-hidden="true"></span>{release}</p></div>
</section>
<figure class="catalog-preview wrap"><div class="preview-header"><span>THE CATALOG / ON HARDWARE</span><span>MPC LIVE II</span></div><img src="assets/manager-catalog.png" alt="Plugin Manager USB catalog running on an MPC Live II, showing plugin cards and Catalog, Local, On MPC and History tabs" width="1280" height="800"><figcaption>Catalog on the MPC Live II · captured 07-10-2026. Catalog contents change.</figcaption></figure>
<section class="compatibility wrap" aria-labelledby="compat-title"><div><p class="eyebrow">VERIFIED HARDWARE</p><h2 id="compat-title">{compatibility}</h2></div></section>
<section class="workflow wrap" aria-labelledby="workflow-title"><div class="section-heading"><p class="eyebrow">FROM STICK TO SESSION</p><h2 id="workflow-title">A straightforward workflow.</h2><p>A prepared bundle needs no programming tools, SSH, separate rooting procedure or computer connection to operate on the MPC.</p>{session_notice}</div><ol class="steps"><li><span>01</span><h3>Prepare the stick</h3><p>{preparation} Copy its complete contents to the USB root, including the hidden marker. Prepare the SD content location in the guide.</p></li><li><span>02</span><h3>Open on the MPC</h3><p>Save your project, insert the drive and choose Open in the stock script prompt.</p></li><li><span>03</span><h3>Choose your plugins</h3><p>Use the online catalog with MPC internet access, or install compatible local ZIPs offline. Review the operation and confirm installation onto the SD card.</p></li><li><span>04</span><h3>Return to making music</h3><p>Return to MPC. Hold Shift + Menu for two seconds to reopen while the stick remains inserted.</p></li></ol></section>
<section class="workspace-section wrap" aria-labelledby="views-title"><div class="section-heading"><p class="eyebrow">FOUR VIEWS / ONE PLACE</p><h2 id="views-title">Know what you have.</h2></div><dl class="views"><div><dt>{catalog_icon}Catalog</dt><dd>Community packages, with compatibility checks before download.</dd></div><div><dt>{local_icon}Local</dt><dd>Install compatible ZIP packages from your stick’s local-plugins folder.</dd></div><div><dt>{installed_icon}On MPC</dt><dd>See actual installed content and choose updates or uninstall.</dd></div><div><dt>{history_icon}History</dt><dd>Keep the stick’s saved inventory and compare it with the current MPC.</dd></div></dl></section>
<section class="development wrap" aria-labelledby="development-title"><div><p class="eyebrow">FOR DEVELOPERS</p><h2 id="development-title">Understand the source.<br>Build your own changes.</h2><p>The developer FAQ covers launch mechanics, checksums, storage, recovery, test fixtures and release preparation.</p><a class="button" href="developer.html">Open the developer FAQ <span aria-hidden="true">→</span></a></div><div class="development-note"><h3>Development setup</h3>{host}</div></section>
'''.format(compatibility=renderer.inline(compatibility[1]), host=renderer.render(host, paragraph_breaks=False), session_notice=renderer.render(session_notice), actions=actions, release=release, preparation=preparation, intro="Install community plugins directly on your MPC from a USB stick. Browse, install, update and remove plugins in one place.", catalog_icon=manager_icon("catalog"), local_icon=manager_icon("local"), installed_icon=manager_icon("installed"), history_icon=manager_icon("history"))


def build(output: Path, repository: str = "", revision: str = "main", root: Path = ROOT) -> None:
    if repository:
        repository_name(repository)
    revision_name(revision)
    if output.is_symlink():
        raise ValueError("site output must not be a symlink")
    output = output.resolve()
    root = root.resolve()
    if output == root or root.is_relative_to(output):
        raise ValueError("site output must not contain the source checkout")
    if output.exists():
        index = output / "index.html"
        owned_titles = ("<title>Plugin Manager USB</title>", "<title>Plugin Manager USB · Twisted Cable</title>")
        if not index.is_file() or not any(title in index.read_text(encoding="utf-8") for title in owned_titles):
            raise ValueError("refusing to replace an existing directory without this site's output")
        shutil.rmtree(output)
    (output / "assets").mkdir(parents=True)
    for asset, destination in ASSETS.items():
        shutil.copyfile(root / "site" / asset, output / destination)
    style_version = hashlib.sha256((output / "assets/styles.css").read_bytes()).hexdigest()[:12]
    for notice, destination in NOTICES.items():
        shutil.copyfile(root / notice, output / destination)
    readme = (root / "README.md").read_text(encoding="utf-8")
    (output / "index.html").write_text(shell(
        "Plugin Manager USB", "USB plugin installer and catalog for MPC standalone.",
        "index.html", landing(readme, root, repository, revision), repository, style_version,
    ), encoding="utf-8")
    for filename, (page, title) in DOCUMENTS.items():
        text = (root / filename).read_text(encoding="utf-8")
        if filename == "README.md":
            text = "\n".join(line for line in text.splitlines() if not re.fullmatch(
                r"\[!\[[^]]*\]\(https://img\.shields\.io/[^)]*\)\]\([^)]*\)", line
            ))
            text = re.sub(
                r'<picture><img src="(site/assets/tab-(?:catalog|local|installed|history)\.svg)" width="16" height="16" alt=""></picture>',
                r'![](\1)', text,
            )
        renderer = Markdown(root, repository, revision)
        content = render_faq(renderer, text) if filename == "DEVELOPER_FAQ.md" else renderer.render(text)
        headings = [h for h in renderer.headings if h.level == 2]
        toc = "".join('<a href="#{}" title="{}">{}</a>'.format(
            html.escape(h.anchor, quote=True), html.escape(h.title, quote=True),
            html.escape(h.title.split(" — ", 1)[0] if filename == "CHANGELOG.md" else SIDEBAR_LABELS[h.anchor]),
        ) for h in headings)
        search = '<div class="faq-search"><label for="faq-search">Search the developer FAQ</label><div class="search-controls"><input id="faq-search" type="search" placeholder="Try checksums, firmware or uninstall" autocomplete="off" aria-describedby="search-status"><button class="search-clear" type="button">Clear</button></div><p id="search-status" role="status" aria-live="polite"></p><p id="no-results" hidden>No matching questions. Try another term or clear the search.</p></div>' if filename == "DEVELOPER_FAQ.md" else ""
        body = '<div class="docs-layout wrap"><aside class="doc-nav" aria-label="On this page">{}</aside><div class="doc-main">{}<article class="prose">{}</article></div></div>'.format(toc, search, content)
        (output / page).write_text(shell(title, title + " for the Plugin Manager USB source.", page, body, repository, style_version), encoding="utf-8")
    (output / ".nojekyll").write_text("", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Build the documentation-only GitHub Pages website.")
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "site")
    parser.add_argument("--repository", type=repository_name, default=None, help="OWNER/REPO for source links")
    parser.add_argument("--revision", type=revision_name, default="main", help="source branch or commit for source links")
    args = parser.parse_args()
    build(args.output, args.repository or "", args.revision)
    print("Website built: " + str(args.output.resolve()))


if __name__ == "__main__":
    main()
