#!/usr/bin/env python3
"""Render spec/rcp-1.0.md -> docs/spec/rcp-1.0.html with a table of contents.

Zero dependencies (stdlib only). Handles the Markdown subset the spec uses:
ATX headings, fenced code blocks, pipe tables, ordered/unordered lists,
blockquotes, and inline code / bold / italics / links. Run from the repo root:

    python3 docs/build_spec.py
"""
import html
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "spec", "rcp-1.0.md")
OUT = os.path.join(ROOT, "docs", "spec", "rcp-1.0.html")

_INLINE_CODE = re.compile(r"`([^`]+)`")
_BOLD = re.compile(r"\*\*([^*]+)\*\*")
_ITALIC = re.compile(r"(?<![\w*])\*([^*\n]+)\*(?![\w*])")
_LINK = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")


def slug(text):
    s = re.sub(r"[^\w\s-]", "", text.lower()).strip()
    return re.sub(r"[\s]+", "-", s)


def inline(text):
    text = html.escape(text)
    # links first (before escaping would break, but we escaped; re-handle the & in urls)
    text = _LINK.sub(lambda m: f'<a href="{html.unescape(m.group(2))}">{m.group(1)}</a>', text)
    text = _INLINE_CODE.sub(lambda m: f"<code>{m.group(1)}</code>", text)
    text = _BOLD.sub(lambda m: f"<strong>{m.group(1)}</strong>", text)
    text = _ITALIC.sub(lambda m: f"<em>{m.group(1)}</em>", text)
    return text


def render(md):
    out, toc = [], []
    lines = md.split("\n")
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]

        # fenced code block
        if line.startswith("```"):
            lang = line[3:].strip()
            i += 1
            buf = []
            while i < n and not lines[i].startswith("```"):
                buf.append(lines[i])
                i += 1
            i += 1  # skip closing fence
            code = html.escape("\n".join(buf))
            out.append(f'<pre class="code" data-lang="{html.escape(lang)}"><code>{code}</code></pre>')
            continue

        # heading
        m = re.match(r"^(#{1,6})\s+(.*)$", line)
        if m:
            level = len(m.group(1))
            text = m.group(2).strip()
            sid = slug(text)
            if level in (2, 3):
                toc.append((level, text, sid))
            out.append(f'<h{level} id="{sid}">{inline(text)}</h{level}>')
            i += 1
            continue

        # table (a header row followed by a |---| separator)
        if line.lstrip().startswith("|") and i + 1 < n and re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[i + 1]):
            header = [c.strip() for c in line.strip().strip("|").split("|")]
            i += 2
            rows = []
            while i < n and lines[i].lstrip().startswith("|"):
                rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")])
                i += 1
            th = "".join(f"<th>{inline(c)}</th>" for c in header)
            trs = "".join("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>" for r in rows)
            out.append(f"<table><thead><tr>{th}</tr></thead><tbody>{trs}</tbody></table>")
            continue

        # blockquote
        if line.startswith(">"):
            buf = []
            while i < n and lines[i].startswith(">"):
                buf.append(lines[i][1:].strip())
                i += 1
            out.append(f"<blockquote>{inline(' '.join(buf))}</blockquote>")
            continue

        # unordered list
        if re.match(r"^\s*[-*]\s+", line):
            buf = []
            while i < n and re.match(r"^\s*[-*]\s+", lines[i]):
                buf.append(re.sub(r"^\s*[-*]\s+", "", lines[i]))
                i += 1
            items = "".join(f"<li>{inline(x)}</li>" for x in buf)
            out.append(f"<ul>{items}</ul>")
            continue

        # ordered list
        if re.match(r"^\s*\d+\.\s+", line):
            buf = []
            while i < n and re.match(r"^\s*\d+\.\s+", lines[i]):
                buf.append(re.sub(r"^\s*\d+\.\s+", "", lines[i]))
                i += 1
            items = "".join(f"<li>{inline(x)}</li>" for x in buf)
            out.append(f"<ol>{items}</ol>")
            continue

        # blank line
        if not line.strip():
            i += 1
            continue

        # paragraph (gather until blank / block start)
        buf = [line]
        i += 1
        while i < n and lines[i].strip() and not re.match(r"^(#{1,6}\s|```|>|\s*[-*]\s|\s*\d+\.\s|\s*\|)", lines[i]):
            buf.append(lines[i])
            i += 1
        out.append(f"<p>{inline(' '.join(buf))}</p>")

    return "\n".join(out), toc


def toc_html(toc):
    items = []
    for level, text, sid in toc:
        cls = "toc-2" if level == 2 else "toc-3"
        items.append(f'<a class="{cls}" href="#{sid}">{html.escape(text)}</a>')
    return "\n".join(items)


PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>RCP/1 Specification — Retrieval Context Protocol</title>
<meta name="description" content="The formal specification of RCP/1, the Retrieval Context Protocol." />
<link rel="stylesheet" href="../assets/style.css" />
<link rel="stylesheet" href="../assets/spec.css" />
</head>
<body class="spec-body">
<header class="spec-header">
  <a class="brand" href="../index.html">RCP</a>
  <div class="links">
    <a href="../index.html">Home</a>
    <a href="https://github.com/1ay1/rcp/blob/main/spec/rcp-1.0.md">Markdown source</a>
    <a href="https://github.com/1ay1/rcp">GitHub</a>
  </div>
</header>
<div class="spec-layout">
  <nav class="spec-toc">
    <div class="toc-title">Contents</div>
    {toc}
  </nav>
  <main class="spec-content">
    {body}
  </main>
</div>
</body>
</html>
"""


def main():
    with open(SRC, encoding="utf-8") as f:
        md = f.read()
    body, toc = render(md)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(PAGE.format(toc=toc_html(toc), body=body))
    # Mirror the canonical schema into docs/ so GitHub Pages (which serves only
    # from /docs) can resolve the schema URL. /schema remains the source of truth.
    import shutil
    schema_src = os.path.join(ROOT, "schema", "rcp-1.0.json")
    schema_dst = os.path.join(ROOT, "docs", "schema", "rcp-1.0.json")
    os.makedirs(os.path.dirname(schema_dst), exist_ok=True)
    shutil.copyfile(schema_src, schema_dst)
    print(f"wrote {OUT}  ({len(toc)} sections); mirrored schema -> docs/schema/")


if __name__ == "__main__":
    sys.exit(main())
