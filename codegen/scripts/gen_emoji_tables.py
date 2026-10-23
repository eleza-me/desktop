#!/usr/bin/env python3
"""Telegram's emoji data + search words  ->  lib/ui/ui/emoji/emoji_tables_auto.cpp

The pictures are Eleza/Resources/emoji/emoji.pack (Telegram Android's 64x64 emoji, format
«EPK3», read by lib/ui/ui/emoji/emoji_pack_reader.cpp). This script writes the DATA that goes
with it; the code that reads it is our own (lib/ui/ui/emoji):

  pages     every emoji of the pack in the pack's order: record (page, index) = pages[page][index],
            written without U+FE0F
  panel     the emoji panel's eight categories, in Telegram's order
  colored   the emoji that take a skin tone in the panel
  aliases   old spellings -> the spelling the pack has
  FE0F      the symbols (BMP) whose emoji form is written with U+FE0F after them
  data      the symbols (BMP, U+00A9..U+3299) that are emoji of the pack on their own
  keywords  emoji -> the words it is searched by (Persian first, then English: CLDR)

Inputs, one of:
  --telegram EmojiData.java   Telegram Android's TMessagesProj/src/main/java/org/telegram/
                              messenger/EmojiData.java, from the same revision as the pack
  --android EmojiTables.java  the Android client's app/src/main/java/me/eleza/app/emoji/
                              EmojiTables.java (generated from the same EmojiData.java by
                              its tools/gen_emoji_tables.py)
and for the search words:
  --keywords EmojiData.json   the iOS client's Packages/Core/Sources/Core/Resources/
                              EmojiData.json (its Scripts/gen_emoji_data.py builds it from CLDR)

usage: gen_emoji_tables.py (--telegram FILE | --android FILE) --keywords FILE [--out FILE]

The pages must come from the same Telegram revision as the pack: tests/ui/test_emoji.cpp
compares the counts and decodes every emoji.
"""
import argparse
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
OUT = ROOT / "lib" / "ui" / "ui" / "emoji" / "emoji_tables_auto.cpp"
# Code units per string literal: far below MSVC's limits for one literal.
CHUNK_UNITS = 3000


# ---- Telegram's EmojiData.java

def block(src, name):
    i = src.index(name)
    j = src.index("{", i)
    depth = 0
    for k in range(j, len(src)):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[j:k + 1]
    raise SystemExit("unterminated " + name)


def unescape(s):
    s = re.sub(r"\\u([0-9a-fA-F]{4})", lambda m: chr(int(m.group(1), 16)), s)
    # Java source holds UTF-16: join surrogate pairs written as escapes
    return s.encode("utf-16", "surrogatepass").decode("utf-16")


def strings(b):
    return [unescape(m.group(1)) for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', b)]


def pages(b):
    out = []
    pos = 1
    entry = re.compile(r"\s*(null|new String\[\]\s*\{)\s*,?")
    while True:
        m = entry.match(b, pos)
        if not m:
            break
        if m.group(1) == "null":
            out.append(None)
            pos = m.end()
            continue
        j = m.end() - 1
        while b[j] != "{":
            j -= 1
        depth = 0
        for k in range(j, len(b)):
            if b[k] == "{":
                depth += 1
            elif b[k] == "}":
                depth -= 1
                if depth == 0:
                    out.append(strings(b[j:k + 1]))
                    pos = k + 1
                    m2 = re.compile(r"\s*,").match(b, pos)
                    if m2:
                        pos = m2.end()
                    break
    return out


def chars(b):
    return [chr(int(x, 16)) for x in re.findall(r"0x([0-9A-Fa-f]{4})", b)]


def from_telegram(path):
    src = pathlib.Path(path).read_text(encoding="utf-8")
    data = pages(block(src, "String[][] data ="))
    colored_pages = pages(block(src, "String[][] dataColored ="))
    if len(data) != 8 or len(colored_pages) != 8:
        raise SystemExit("expected 8 pages, got %d / %d" % (len(data), len(colored_pages)))
    return {
        "pages": data,
        "panel": [p if p is not None else data[i] for i, p in enumerate(colored_pages)],
        "colored": strings(block(src, "String[] emojiColored =")),
        "alias_old": strings(block(src, "String[] aliasOld =")),
        "alias_new": strings(block(src, "String[] aliasNew =")),
        "fe0f": "".join(chars(block(src, "char[] emojiToFE0F ="))),
        "data_chars": "".join(chars(block(src, "char[] dataChars ="))),
    }


# ---- the Android client's EmojiTables.java (the same data, already extracted)

def from_android(path):
    src = pathlib.Path(path).read_text(encoding="utf-8")

    def array(name):
        m = re.search(r"String\[\] %s = \{(.*?)\};" % name, src, re.S)
        if not m:
            raise SystemExit("no " + name)
        items = []
        for chunk in strings(m.group(1)):
            items.extend(x for x in chunk.split(" ") if x)
        return items

    def constant(name):
        m = re.search(r'String %s = "((?:[^"\\]|\\.)*)";' % name, src)
        if not m:
            raise SystemExit("no " + name)
        return unescape(m.group(1))

    count = int(re.search(r"PAGE_COUNT = (\d+);", src).group(1))
    return {
        "pages": [array("PAGE_%d" % i) for i in range(count)],
        "panel": [array("PANEL_%d" % i) for i in range(count)],
        "colored": array("COLORED"),
        "alias_old": array("ALIAS_OLD"),
        "alias_new": array("ALIAS_NEW"),
        "fe0f": constant("TO_FE0F"),
        "data_chars": constant("DATA_CHARS"),
    }


# ---- output

def literal(s):
    """A u"" literal: ASCII as is, Arabic-script letters raw (readable), the rest escaped."""
    out = []
    for ch in s:
        c = ord(ch)
        if 0x20 <= c < 0x7F and ch not in '\\"?':
            out.append(ch)
        elif ch == "?":
            out.append("\\?")  # (no trigraphs)
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\n":
            out.append("\\n")
        elif 0x0621 <= c <= 0x06D3 and c not in range(0x064B, 0x0670):
            out.append(ch)
        elif c > 0xFFFF:
            out.append("\\U%08X" % c)
        else:
            out.append("\\u%04X" % c)
    return 'u"' + "".join(out) + '"'


def units(s):
    return len(s.encode("utf-16-le")) // 2


def chunked(items, separator):
    """Joins items with `separator` into pieces of at most CHUNK_UNITS code units each."""
    chunks = []
    current = []
    size = 0
    for it in items:
        n = units(it) + 1
        if current and size + n > CHUNK_UNITS:
            chunks.append(separator.join(current))
            current = []
            size = 0
        current.append(it)
        size += n
    if current:
        chunks.append(separator.join(current))
    return chunks


def list_array(name, items, out):
    for it in items:
        assert it and " " not in it, repr(it)
    chunks = chunked(items, " ")
    out.append("const char16_t *const %sChunks[] = {\n" % name)
    for c in chunks:
        out.append("\t%s,\n" % literal(c))
    out.append("};\n\n")
    return "{ %sChunks, %d, %d }" % (name, len(chunks), len(items))


def keyword_lines(path, known):
    data = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    lines = []
    for emoji, words in data["keywords"].items():
        key = emoji.replace("️", "").replace("︎", "")
        if key not in known:
            continue
        words = "|".join(w.strip() for w in words.split("|") if w.strip())
        assert "\t" not in words and "\n" not in words
        if words:
            lines.append(key + "\t" + words)
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--telegram")
    source.add_argument("--android")
    parser.add_argument("--keywords", required=True)
    parser.add_argument("--out", default=str(OUT))
    args = parser.parse_args()

    t = from_telegram(args.telegram) if args.telegram else from_android(args.android)
    if len(t["alias_old"]) != len(t["alias_new"]):
        raise SystemExit("alias lists differ in length")
    strip = lambda s: s.replace("️", "").replace("︎", "")
    known = {strip(e) for page in t["pages"] for e in page}
    keywords = keyword_lines(args.keywords, known)

    out = []
    out.append("// Generated by codegen/scripts/gen_emoji_tables.py from Telegram's emoji data and the\n"
               "// CLDR search words — do not edit. See the script for the sources.\n")
    out.append('#include "ui/emoji/emoji_tables.h"\n\n')
    out.append("namespace Ui::Emoji::Tables {\n")
    out.append("namespace {\n\n")
    page_refs = [list_array("kPage%d" % i, p, out) for i, p in enumerate(t["pages"])]
    panel_refs = [list_array("kPanel%d" % i, p, out) for i, p in enumerate(t["panel"])]
    colored_ref = list_array("kColored", t["colored"], out)
    old_ref = list_array("kAliasOld", t["alias_old"], out)
    new_ref = list_array("kAliasNew", t["alias_new"], out)
    kw_chunks = chunked(keywords, "\n")
    out.append("const char16_t *const kKeywordsChunks[] = {\n")
    for c in kw_chunks:
        out.append("\t%s,\n" % literal(c))
    out.append("};\n\n")
    out.append("} // namespace\n\n")
    out.append("const List kPages[kPageCount] = {\n%s};\n\n" % "".join("\t%s,\n" % r for r in page_refs))
    out.append("const List kPanel[kPageCount] = {\n%s};\n\n" % "".join("\t%s,\n" % r for r in panel_refs))
    out.append("const List kColored = %s;\n" % colored_ref)
    out.append("const List kAliasOld = %s;\n" % old_ref)
    out.append("const List kAliasNew = %s;\n\n" % new_ref)
    out.append("const List kKeywords = { kKeywordsChunks, %d, %d };\n\n" % (len(kw_chunks), len(keywords)))
    out.append("const char16_t kToFe0f[] = %s;\n" % literal(t["fe0f"]))
    out.append("const char16_t kDataChars[] = %s;\n\n" % literal(t["data_chars"]))
    out.append("} // namespace Ui::Emoji::Tables\n")
    if len(t["pages"]) != 8:
        raise SystemExit("expected 8 pages")
    pathlib.Path(args.out).write_text("".join(out), encoding="utf-8", newline="\n")
    print("pages", [len(p) for p in t["pages"]], "panel", [len(p) for p in t["panel"]],
          "colored", len(t["colored"]), "aliases", len(t["alias_old"]), "keywords", len(keywords),
          "->", args.out)


if __name__ == "__main__":
    sys.exit(main())
