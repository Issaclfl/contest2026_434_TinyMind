#!/usr/bin/env python3
"""Extract the hold-to-talk page out of voice_exec.c and check its JavaScript.

Why this exists: the page lives inside a C string literal, so the C compiler is
the only thing that ever looks at it.  A typo in the JavaScript (an unbalanced
brace, a missing quote) compiles perfectly and shows up as "the button does
nothing" on a phone -- which is exactly the failure this page was written to
fix.  This script takes the literal apart the way the C compiler would, then
hands the <script> body to `node --check`, so a broken page fails at build/dev
time instead of in someone's hand.

    python tools/check_voice_page.py            # parse and syntax-check
    python tools/check_voice_page.py --html out.html   # also write the page

Exit code 0 means: every C literal was well formed, the page has a <script>
block, and node accepted it.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
SRC = HERE.parent / "main" / "voice_exec.c"
DECL = "static const char kVoicePage[] ="

# C escapes we actually use in that literal, mapped to what the compiler makes
# of them.  Applied while scanning, one escape at a time -- collecting the pairs
# and unescaping afterwards is how this script first got it wrong (it handed
# node a page containing a literal \n where the device sends a newline).
C_ESCAPES = {"\\": "\\", '"': '"', "n": "\n", "t": "\t", "r": "\r", "'": "'"}


def c_literal_body(text):
    """The concatenated string, as the C compiler would see it.

    Comments are skipped, and they have to be: a comment inside this statement
    may contain quotes (the page's own comments talk about 麦克风权限), and
    treating those as string literals is what first shipped a page with the
    comment text spliced into the middle of the JavaScript.
    """
    start = text.index(DECL)
    parts, i, n = [], start + len(DECL), len(text)
    seen_literal = False
    while i < n:
        ch = text[i]
        if not seen_literal:
            if ch == ";" and parts:
                break
            if ch == "/" and text.startswith("/*", i):
                end = text.find("*/", i + 2)
                if end < 0:
                    raise SystemExit("unterminated /* comment in %s" % SRC)
                i = end + 2
                continue
            if ch == "/" and text.startswith("//", i):
                end = text.find("\n", i + 2)
                i = n if end < 0 else end + 1
                continue
            if ch != '"':
                i += 1
                continue
        if ch == '"':
            seen_literal = True
            j, buf = i + 1, []
            while j < n:
                c = text[j]
                if c == "\\":
                    if j + 1 >= n:
                        raise SystemExit("dangling backslash in %s" % SRC)
                    e = text[j + 1]
                    buf.append(C_ESCAPES.get(e, e))
                    j += 2
                    continue
                if c == '"':
                    break
                buf.append(c)
                j += 1
            if j >= n:
                raise SystemExit("unterminated string literal in %s" % SRC)
            parts.append("".join(buf))
            i, seen_literal = j + 1, False
            continue
        i += 1
    return "".join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--html", default=None, help="也把整页写到这个文件")
    ap.add_argument("--js", default=None, help="也把 <script> 内容写到这里")
    a = ap.parse_args()

    body = c_literal_body(SRC.read_text(encoding="utf-8"))
    problems = []

    for tag in ("<!doctype html>", "</html>", "<script>", "</script>"):
        if tag not in body:
            problems.append("页面里找不到 %s" % tag)
    if not body.count("<script>") == body.count("</script>") == 1:
        problems.append("script 标签不是一个整块")

    js = ""
    m = re.search(r"<script>(.*?)</script>", body, re.S)
    if m:
        js = m.group(1)

    ids_used = set(re.findall(r"\$\('([a-zA-Z0-9_]+)'\)", js))
    ids_have = set(re.findall(r"id='([a-zA-Z0-9_]+)'", body))
    for missing in sorted(ids_used - ids_have):
        problems.append("JS 引用了页面上不存在的 id: %s" % missing)

    if a.html:
        pathlib.Path(a.html).write_text(body, encoding="utf-8")
        print("写 %s（%d B）" % (a.html, len(body.encode("utf-8"))))
    if a.js:
        pathlib.Path(a.js).write_text(js, encoding="utf-8")

    with tempfile.TemporaryDirectory() as tmp:
        f = pathlib.Path(tmp) / "page.js"
        f.write_text(js, encoding="utf-8")
        try:
            r = subprocess.run(["node", "--check", str(f)], capture_output=True, text=True)
        except FileNotFoundError:
            r = None
        if r is None:
            print("没找到 node，跳过语法检查")
        elif r.returncode == 0:
            print("node --check 通过（%d B 的 JS）" % len(js.encode("utf-8")))
        else:
            problems.append("node --check 失败：\n" + (r.stderr or r.stdout).strip())

    if problems:
        print("\n有问题：")
        for p in problems:
            print("  - " + p)
        return 1
    print("页面看起来是好的")
    return 0


if __name__ == "__main__":
    sys.exit(main())
