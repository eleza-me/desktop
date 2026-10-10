#!/usr/bin/env python3
"""design/icons.json + design/extra_icons.json  ->  Eleza/Resources/icons/*.svg + qrc/icons.qrc

Each icon body (SVG fragments on a 24x24 grid, the shorthand ` S/>` standing for the common
stroke attributes) becomes one standalone .svg. Colors stay the design's literals; the icon
runtime (lib/ui/ui/style/style_core_icon.cpp) substitutes them when rasterising:
    #161A17  -> the icon's own color (the tint given in the .style file)
    #018A54  -> st::brandPrimary       (the filled «pupil» of `.active` icons)
    #4D524B  -> st::textSecondary
    white    -> st::staticWhite
File names: `name.svg`; the `.active` state is written as `name_active.svg`
(use icon {{ "name_active", color }} in .style files).

usage: gen_icons.py [icons.json] [extra_icons.json]
"""
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
icons_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "design" / "icons.json"
extra_path = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "design" / "extra_icons.json"
out_dir = ROOT / "Eleza" / "Resources" / "icons"
qrc_path = ROOT / "Eleza" / "Resources" / "qrc" / "icons.qrc"

STROKE = 'stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"'

icons = json.loads(icons_path.read_text(encoding="utf-8"))
extra = json.loads(extra_path.read_text(encoding="utf-8")) if extra_path.exists() else {}
for key in extra:
    if key in icons:
        raise SystemExit(f"extra icon '{key}' already exists in icons.json")
icons.update(extra)

out_dir.mkdir(parents=True, exist_ok=True)
for old in out_dir.glob("*.svg"):
    old.unlink()

names = []
for key, body in icons.items():
    name = key.replace(".active", "_active")
    body = body.replace(" S/>", " " + STROKE + "/>").replace('"white"', '"#FFFFFF"')
    svg = (
        '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none">'
        + body
        + "</svg>\n"
    )
    (out_dir / (name + ".svg")).write_text(svg, encoding="utf-8")
    names.append(name)

names.sort()
qrc = ["<RCC>", '\t<qresource prefix="/icons">']
qrc += [f'\t\t<file alias="{n}.svg">../icons/{n}.svg</file>' for n in names]
qrc += ["\t</qresource>", "</RCC>", ""]
qrc_path.parent.mkdir(parents=True, exist_ok=True)
qrc_path.write_text("\n".join(qrc), encoding="utf-8")
print("wrote", len(names), "icons to", out_dir, "and", qrc_path)
