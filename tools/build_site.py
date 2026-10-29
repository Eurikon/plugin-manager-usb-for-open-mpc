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
    "welcome-screen": "Welcome",
    "install-the-manager-on-sd": "SD Setup",
    "setup-and-manager-updates": "SETUP",
    "opening-and-recovery": "Recovery",
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
    "assets/favicon.svg": "assets/favicon.svg", "assets/favicon.ico": "assets/favicon.ico",
    "styles.css": "assets/styles.css", "site.js": "assets/site.js", "assets/manager-catalog.png": "assets/manager-catalog.png",
    **{"assets/tab-{}.svg".format(kind): "assets/tab-{}.svg".format(kind) for kind in ("catalog", "local", "installed", "history")},
    **{"assets/manager-{}.png".format(page): "assets/manager-{}.png".format(page) for page in ("welcome", "setup", "local", "on-mpc", "history", "sd-removal")},
}
NOTICES = {"LICENSE": "license.txt", "licenses/community-tools.txt": "community-tools-license.txt"}


def asset_url(asset: str, root: Path) -> str:
    version = hashlib.sha256((root / "site" / asset).read_bytes()).hexdigest()[:12]
    return ASSETS[asset] + "?v=" + version


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
                asset_url(source, self.root), html.escape(match[1], quote=True)
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


def site_icon(name: str) -> str:
    shapes = {
        'overview': '<rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/>',
        'guide': '<path d="M12 5v15M12 5C9 3 5 3 2 4v15c3-1 7-1 10 1 3-2 7-2 10-1V4c-3-1-7-1-10 1Z"/>',
        'code': '<path d="m8 6-6 6 6 6m8-12 6 6-6 6m-3-15-2 18"/>',
        'history': '<path d="M4 8a9 9 0 1 1-1 8M4 2v6h6m2-1v6l4 2"/>',
        'download': '<path d="M12 3v12m-5-5 5 5 5-5M3 16v5h18v-5"/>',
        'arrow': '<path d="M4 12h16m-6-6 6 6-6 6"/>',
        'sun': '<circle cx="12" cy="12" r="4"/><path d="M12 1v3m0 16v3M1 12h3m16 0h3M4 4l2 2m12 12 2 2M4 20l2-2M18 6l2-2"/>',
        'moon': '<path d="M21 14A9 9 0 0 1 10 3a9 9 0 1 0 11 11Z"/>',
    }
    return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' + shapes[name] + '</svg>'


PAGE_NAVIGATION = [('index.html', 'Overview', 'overview'), ('guide.html', 'User guide', 'guide'), ('developer.html', 'Developer FAQ', 'code'), ('changelog.html', 'Changelog', 'history')]
SCREEN_PAGES = [
    ('welcome', 'Welcome', 'Choose how to open the manager, install to SD or use the on-track OpenMPC Plugin Manager G2 instead.', 'setup', 'guide.html#welcome-screen', 'Opening'),
    ('catalog', 'Catalog', 'Browse community releases and check compatibility before downloading.', 'browse', 'guide.html#find-update-and-remove-plugins', 'Online'),
    ('local', 'Local', 'Install compatible ZIPs from the source drive’s local-plugins folder.', 'browse', 'guide.html#find-update-and-remove-plugins', 'Offline'),
    ('on-mpc', 'On MPC', 'See what is installed on the device and choose update or removal actions.', 'browse', 'guide.html#find-update-and-remove-plugins', 'Device'),
    ('history', 'History', 'Compare the saved installation record with the current device.', 'browse', 'guide.html#find-update-and-remove-plugins', 'Inventory'),
    ('setup', 'Setup', 'Manage the SD installation, welcome message and manager updates.', 'setup', 'guide.html#setup-and-manager-updates', 'USB & SD'),
]


def shell(title: str, description: str, current: str, body: str, repository: str, style_version: str, sections: str = "") -> str:
    document_title = title if current == "index.html" else title + " · Plugin Manager USB"
    page_title, glyph = next((label, symbol) for page, label, symbol in PAGE_NAVIGATION if page == current)
    navigation = "".join('<a href="{}"{}>{}<span>{}</span></a>'.format(
        page, ' aria-current="page"' if page == current else "", site_icon(symbol), label,
    ) for page, label, symbol in PAGE_NAVIGATION)
    if sections:
        sections = '<nav class="doc-nav sidebar-sections" aria-label="Page sections">' + sections + '</nav>'
    else:
        sections = '<div class="sidebar-label">Get started</div><nav class="sidebar-sections" aria-label="Setup guides"><a href="guide.html#mpc-os-391">MPC OS 3.9.1</a><a href="guide.html#mpc-os-310-first-use">MPC OS 3.10</a><a href="guide.html#install-the-manager-on-sd">Install on SD</a><a href="guide.html#opening-and-recovery">Opening &amp; recovery</a></nav>'
    if repository:
        download = '<a class="button primary sidebar-download" href="https://github.com/{}/releases/latest">{} Download installer</a>'.format(repository, site_icon("download"))
        source = '<a class="sidebar-source" href="https://github.com/{}">{} Source on GitHub {}</a>'.format(repository, site_icon("code"), site_icon("arrow"))
    else:
        download = '<a class="button primary sidebar-download" href="guide.html#using-a-prepared-usb-bundle">{} USB setup guide</a>'.format(site_icon("guide"))
        source = '<span class="sidebar-source">Local preview: no installer download link configured.</span>'
    return '''<!doctype html>
<html lang="en" data-theme="dark">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="dark light">
  <meta name="description" content="{description}">
  <title>{title}</title>
  <link rel="icon" href="assets/favicon.ico" sizes="32x32">
  <link rel="icon" href="assets/favicon.svg" type="image/svg+xml" sizes="any">
  <link rel="stylesheet" href="assets/styles.css?v={style_version}">
  <script src="assets/site.js" defer></script>
</head>
<body>
<a class="skip-link" href="#content">Skip to content</a>
<aside class="site-sidebar" aria-label="Site navigation">
  <a class="sidebar-brand" href="index.html" aria-label="Plugin Manager USB overview"><img src="assets/favicon.svg" width="32" height="32" alt=""><span class="title-pill"><span>PLUGIN MANAGER</span><b>USB</b></span></a>
  <div class="sidebar-scroll"><nav class="sidebar-nav" aria-label="Main navigation">{navigation}</nav><div class="sidebar-divider"></div>{sections}</div>
  <div class="sidebar-footer">{download}{source}</div>
</aside>
<header class="site-toolbar"><div class="toolbar-title">{page_icon}<span>{page_title}</span></div><button class="theme-toggle" type="button" aria-label="Switch to light theme" aria-pressed="false"><span class="theme-symbol theme-sun">{sun}</span><span class="theme-symbol theme-moon">{moon}</span><span class="theme-label">Light</span></button></header>
<main id="content" tabindex="-1">{body}</main>
<footer class="site-footer"><div><a href="guide.html#credits-and-acknowledgments">About this project</a></div></footer>
</body>
</html>
'''.format(title=html.escape(document_title), description=html.escape(description, quote=True), navigation=navigation, sections=sections, download=download, source=source, page_icon=site_icon(glyph), page_title=page_title, sun=site_icon("sun"), moon=site_icon("moon"), body=body, style_version=style_version)


def landing(readme: str, root: Path, repository: str, revision: str) -> str:
    compatibility = re.search(r"\*\*(MPC Live .*? / AKAI MPC OS .*?)\*\*", readme)
    if not compatibility:
        raise ValueError("README must state tested hardware compatibility")
    hardware, firmware = compatibility[1].split(" / ", 1)
    additional = re.search(r"\*\*Additional hardware checks:\*\* (.+?)(?:<br>|\n)", readme)
    additional_hardware = '<span>{}</span>'.format(html.escape(additional[1])) if additional else ""
    cards = ''.join('''<a class="screen-card" href="{link}" data-screen data-group="{group}">
<div class="screen-image"><img src="{image}" alt="{title} screen" width="1280" height="800" loading="lazy"></div>
<div class="screen-info"><div class="screen-title"><h3>{title}</h3><span class="screen-tag">{tag}</span></div><p>{description}</p><span class="screen-more">View guide {arrow}</span></div></a>'''.format(image=asset_url("assets/manager-"+key+".png",root),title=title,description=description,group=group,link=link,tag=tag,arrow=site_icon('arrow')) for key,title,description,group,link,tag in SCREEN_PAGES)
    return '''<div class="overview-content">
<section class="overview-intro"><div><p class="overview-kicker">USB / SD · MPC STANDALONE</p><h1>Plugin Manager USB for OPEN MPC</h1><p>Browse, install, update and remove community plugins on your MPC touchscreen.</p></div><a class="button primary" href="guide.html#using-a-prepared-usb-bundle">Get started {arrow}</a></section>
<div class="hardware-strip"><span class="hardware-label">Tested hardware</span><span>{hardware}</span><span>{firmware}</span>{additional_hardware}<a href="guide.html#using-a-prepared-usb-bundle">Firmware setup {arrow}</a></div>
<section class="library" id="library" aria-labelledby="library-title"><div class="library-heading"><div><h2 id="library-title">Inside the manager</h2></div></div>
<div class="library-controls"><div class="screen-filters" role="group" aria-label="Filter screens"><button type="button" data-filter="all" aria-pressed="true">All screens <span>6</span></button><button type="button" data-filter="browse" aria-pressed="false">Browsing <span>4</span></button><button type="button" data-filter="setup" aria-pressed="false">Setup &amp; storage <span>2</span></button></div><p id="screen-count" role="status" aria-live="polite">6 screens</p></div>
<div class="screen-grid">{cards}</div></section>
<section class="setup-routes" aria-labelledby="start-title"><div><h2 id="start-title">One bundle. Two setup routes.</h2><p>Copy the archive contents to an MPC-formatted USB stick, including the hidden marker.</p></div><div class="route-grid"><a href="guide.html#mpc-os-391"><span class="route-number">01</span><div><h3>MPC OS 3.9.1</h3><p>Insert the stick and choose Open in the script prompt.</p></div>{arrow}</a><a href="guide.html#mpc-os-310-first-use"><span class="route-number">02</span><div><h3>MPC OS 3.10</h3><p>Import the supplied helper once, then use USB or install the manager on SD.</p></div>{arrow}</a></div><p class="session-note">Save your project before opening. The MPC application restarts when you return; allow 30 seconds or longer.</p></section>
</div>'''.format(cards=cards,arrow=site_icon('arrow'),hardware=html.escape(hardware),firmware=html.escape(firmware),additional_hardware=additional_hardware)


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
        body = '<div class="docs-layout"><div class="doc-main">{}<article class="prose">{}</article></div></div>'.format(search, content)
        (output / page).write_text(shell(title, title + " for the Plugin Manager USB source.", page, body, repository, style_version, toc), encoding="utf-8")
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
