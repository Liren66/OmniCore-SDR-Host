"""MkDocs hooks for the OmniCore Host documentation site (see mkdocs.yml).

The Markdown in docs/ is written to be read on GitHub and inside the installed package, so it links
to files outside docs/ (examples/, packaging/, README.md). Those files are not part of the site;
this hook points such links at the file on GitHub instead. It also pulls the repository's
CHANGELOG.md into the pages that contain the include marker.
"""
import os
import posixpath
import re

from mkdocs.plugins import event_priority

REPO_BLOB = "https://github.com/Liren66/OmniCore-SDR-Host/blob/main/"
INCLUDE = re.compile(r"<!--\s*include:\s*([^\s]+)\s*-->")
# Markdown link/image targets: ](target) ; raw HTML: src="target" / href="target"
MD_LINK = re.compile(r"(\]\()([^)\s]+)(\))")
HTML_ATTR = re.compile(r"""((?:src|href)=")([^"]+)(")""")
FENCE = re.compile(r"^(```|~~~)")


def _outside_docs(target, page_dir):
    """Return the GitHub URL for a relative target that leaves docs/, else None."""
    if re.match(r"^[a-z][a-z0-9+.-]*:|^#|^/", target, re.I):
        return None
    path, _, frag = target.partition("#")
    if not path:
        return None
    resolved = posixpath.normpath(posixpath.join(page_dir, path))
    if not resolved.startswith("../"):
        return None
    repo_path = posixpath.normpath(posixpath.join("docs", resolved))
    return REPO_BLOB + repo_path + ("#" + frag if frag else "")


def _rewrite(text, page_dir):
    def md(m):
        url = _outside_docs(m.group(2), page_dir)
        return m.group(1) + url + m.group(3) if url else m.group(0)

    def attr(m):
        url = _outside_docs(m.group(2), page_dir)
        return m.group(1) + url + m.group(3) if url else m.group(0)

    out, in_fence = [], False
    for line in text.splitlines(keepends=True):
        if FENCE.match(line.lstrip()):
            in_fence = not in_fence
        if not in_fence:
            line = HTML_ATTR.sub(attr, MD_LINK.sub(md, line))
        out.append(line)
    return "".join(out)


def _include(markdown, config):
    root = os.path.dirname(os.path.abspath(config["config_file_path"]))

    def repl(m):
        with open(os.path.join(root, m.group(1)), encoding="utf-8") as f:
            text = f.read()
        # Drop the file's own title (the page supplies one) and turn repo-root links into docs/ links.
        text = re.sub(r"\A# .*\n", "", text)
        return re.sub(r"\]\(docs/", "](", text)

    return INCLUDE.sub(repl, markdown)


def on_page_markdown(markdown, page, config, files, **kwargs):
    markdown = _include(markdown, config)
    page_dir = posixpath.dirname(page.file.src_uri)
    return _rewrite(markdown, page_dir)


# Language folders that get their own search index. The i18n plugin writes one combined index to
# search/search_index.json after all languages are built; this splits it so a search on an English
# page only returns English pages and vice versa. Chinese pages read zh-CN/search/search_index.json
# because tools/mkdocs/overrides/main.html points their search base at zh-CN/.
SEARCH_LANGUAGE_DIRS = ("zh-CN",)


# The i18n plugin writes the combined index in its own on_post_build at priority -100; run after it.
@event_priority(-200)
def on_post_build(config, **kwargs):
    import json

    path = os.path.join(config["site_dir"], "search", "search_index.json")
    if not os.path.isfile(path):
        return
    with open(path, encoding="utf-8") as f:
        index = json.load(f)
    docs = index.get("docs", [])
    for lang in SEARCH_LANGUAGE_DIRS:
        prefix = lang + "/"
        own = [d for d in docs if d["location"].startswith(prefix)]
        if not own:
            continue  # an inner per-language build, or already split
        docs = [d for d in docs if not d["location"].startswith(prefix)]
        lang_index = dict(index, docs=[dict(d, location=d["location"][len(prefix):]) for d in own])
        lang_dir = os.path.join(config["site_dir"], lang, "search")
        os.makedirs(lang_dir, exist_ok=True)
        with open(os.path.join(lang_dir, "search_index.json"), "w", encoding="utf-8") as f:
            json.dump(lang_index, f, ensure_ascii=False, separators=(",", ":"))
    if len(docs) != len(index.get("docs", [])):
        with open(path, "w", encoding="utf-8") as f:
            json.dump(dict(index, docs=docs), f, ensure_ascii=False, separators=(",", ":"))
