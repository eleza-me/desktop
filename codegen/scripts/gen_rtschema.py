#!/usr/bin/env python3
"""Resources/proto/{views,wire}.proto (+ views.always.json) -> SourceFiles/rt/rt_schema_auto.{h,cpp}

The realtime v2 protobuf messages as static schema tables for the dynamic codec
(rt/rt_proto.cpp) and the RPC -> HTTP bridge (rt/rt_methods.cpp). No protoc and no generated
message classes: the codec encodes and decodes QJsonObject values with the keys the HTTP API
uses (`json_name`).

Parses proto3 text directly; the two files use a small subset: messages, nested messages,
scalar / message fields, repeated, optional, map<k, v>, json_name, oneof. Three conventions of
the backend's generator (cmd/viewprotogen) are read from comments:

    // chat.send - POST /v1/chats/{id}/messages (...)     before `message ChatSendRequest`
    string chat = 1 [...]; // path parameter {id}
    string before = 2 [...]; // query parameter

Copy the three source files from eleza-backend/api/proto/eleza/realtime/v2/ when the backend
changes, then run:

    python3 codegen/scripts/gen_rtschema.py
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "Eleza" / "Resources" / "proto"
DEST = ROOT / "Eleza" / "SourceFiles" / "rt"

SCALARS = {
    "double": "Double", "float": "Double",
    "int64": "Int64", "uint64": "Int64", "sint64": "Int64", "fixed64": "Int64", "sfixed64": "Int64",
    "int32": "Int32", "sint32": "Int32", "sfixed32": "Int32",
    "fixed32": "Uint32", "uint32": "Uint32",
    "bool": "Bool", "string": "String", "bytes": "Bytes",
}

TOKEN = re.compile(r"//[^\n]*|/\*.*?\*/|[A-Za-z_][\w.]*|\d+|[{}=;<>,\[\]()]|\"[^\"]*\"", re.S)
METHOD_COMMENT = re.compile(r"^//\s*([a-z_.]+)\s+\S+\s+(GET|POST|PUT|PATCH|DELETE)\s+(/\S+)")
PATH_COMMENT = re.compile(r"path parameter \{(\w+)\}")


class Field:
    def __init__(self, name, number, ftype, label, json_name):
        self.name = name
        self.number = number
        self.type = ftype
        self.label = label
        self.json = json_name or name
        self.query = False
        self.path = None


def parse(text, messages, methods):
    toks = TOKEN.findall(text)
    i = 0
    last_comment = ""

    def skip_comments():
        nonlocal i, last_comment
        while i < len(toks) and toks[i].startswith(("//", "/*")):
            last_comment = toks[i]
            i += 1

    def tok():
        skip_comments()
        return toks[i]

    def take():
        nonlocal i
        skip_comments()
        value = toks[i]
        i += 1
        return value

    def parse_field():
        nonlocal i
        label = None
        if tok() in ("repeated", "optional"):
            label = take()
        if tok() == "map":
            take()
            assert take() == "<"
            key = take()
            assert take() == ","
            value = take()
            assert take() == ">"
            ftype = ("map", key, value)
        else:
            ftype = take()
        name = take()
        assert take() == "=", name
        number = int(take())
        json_name = None
        if tok() == "[":
            while tok() != "]":
                if take() == "json_name":
                    assert take() == "="
                    json_name = take().strip('"')
            take()
        assert take() == ";", name
        field = Field(name, number, ftype, label, json_name)
        # A trailing comment on the same statement describes the field's place in the route.
        if i < len(toks) and toks[i].startswith("//"):
            comment = toks[i]
            match = PATH_COMMENT.search(comment)
            if match:
                field.path = match.group(1)
            elif "query parameter" in comment:
                field.query = True
        return field

    def parse_message(leading):
        name = take()
        assert take() == "{", name
        fields = []
        while tok() != "}":
            t = tok()
            if t == "message":
                take()
                parse_message("")
            elif t == "oneof":
                take()
                take()
                assert take() == "{"
                while tok() != "}":
                    fields.append(parse_field())
                take()
            elif t in ("option", "reserved", "enum"):
                while take() != ";":
                    pass
            else:
                fields.append(parse_field())
        take()
        messages[name] = fields
        match = METHOD_COMMENT.match(leading)
        if match and name.endswith("Request"):
            methods[match.group(1)] = (match.group(2), match.group(3), name)

    while i < len(toks):
        skip_comments()
        if i >= len(toks):
            break
        if toks[i] == "message":
            leading = last_comment
            i += 1
            parse_message(leading)
        elif toks[i] in ("syntax", "package", "import", "option"):
            while take() != ";":
                pass
        else:
            i += 1


def method_message_name(method, suffix):
    parts = re.split(r"[._]", method)
    return "".join(p[:1].upper() + p[1:] for p in parts if p) + suffix


def main():
    always = json.loads((SRC / "views.always.json").read_text(encoding="utf-8"))
    messages = {}
    methods = {}
    parse((SRC / "views.proto").read_text(encoding="utf-8"), messages, methods)
    parse((SRC / "wire.proto").read_text(encoding="utf-8"), messages, methods)

    names = sorted(messages)
    index = {name: i for i, name in enumerate(names)}

    def short(type_name):
        return type_name.split(".")[-1]

    def is_row(name):
        fields = messages[name]
        return len(fields) == 1 and fields[0].name == "items" and fields[0].label == "repeated"

    def cstr(value):
        return "nullptr" if value is None else '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'

    out = [
        "// Generated by codegen/scripts/gen_rtschema.py from Resources/proto/*.proto - do not edit.",
        '#include "rt/rt_schema_auto.h"',
        "",
        "#include <algorithm>",
        "#include <iterator>",
        "",
        "namespace Rt::Schema {",
        "namespace {",
        "",
        "using K = Kind;",
        "",
    ]
    for name in names:
        fields = messages[name]
        if not fields:
            continue
        out.append(f"constexpr Field k{name}Fields[] = {{")
        for f in fields:
            kind, map_key, map_value, message = "String", "String", "String", -1
            if isinstance(f.type, tuple):
                kind = "Map"
                map_key = SCALARS[f.type[1]]
                if f.type[2] in SCALARS:
                    map_value = SCALARS[f.type[2]]
                else:
                    map_value = "Message"
                    message = index[short(f.type[2])]
            elif f.type in SCALARS:
                kind = SCALARS[f.type]
            else:
                kind = "Message"
                message = index[short(f.type)]
            flags = []
            if f.label == "repeated":
                flags.append("kRepeated")
            if f.label == "optional":
                flags.append("kOptional")
            if f.json in always.get(name, ()):
                flags.append("kAlways")
            if f.query:
                flags.append("kQuery")
            if f.path:
                flags.append("kPath")
            flag_text = " | ".join(flags) if flags else "0"
            out.append(
                f"\t{{ {f.number}, {cstr(f.json)}, K::{kind}, K::{map_key}, K::{map_value}, "
                f"{message}, uchar({flag_text}), {cstr(f.path)} }},")
        out.append("};")
        out.append("")
    out.append("constexpr Message kMessages[] = {")
    for name in names:
        fields = messages[name]
        table = f"k{name}Fields" if fields else "nullptr"
        out.append(f"\t{{ {cstr(name)}, {table}, {len(fields)}, {'true' if is_row(name) else 'false'} }},")
    out.append("};")
    out.append("")
    out.append("constexpr Method kMethods[] = {")
    count = 0
    for method in sorted(methods):
        verb, pattern, request = methods[method]
        expected = method_message_name(method, "Request")
        response = method_message_name(method, "Response")
        if request != expected or response not in index:
            sys.exit(f"method {method}: messages {expected}/{response} do not match the schema")
        out.append(f"\t{{ {cstr(method)}, {cstr(verb)}, {cstr(pattern)}, {index[request]}, {index[response]} }},")
        count += 1
    out.append("};")
    out += [
        "",
        "} // namespace",
        "",
        "const Message *Messages() {",
        "\treturn kMessages;",
        "}",
        "",
        "int MessagesCount() {",
        "\treturn int(std::size(kMessages));",
        "}",
        "",
        "const Method *Methods() {",
        "\treturn kMethods;",
        "}",
        "",
        "int MethodsCount() {",
        "\treturn int(std::size(kMethods));",
        "}",
        "",
        "const Message *MessageAt(int index) {",
        "\treturn (index >= 0 && index < MessagesCount()) ? &kMessages[index] : nullptr;",
        "}",
        "",
        "const Message *FindMessage(QLatin1StringView name) {",
        "\tconst auto begin = std::begin(kMessages);",
        "\tconst auto end = std::end(kMessages);",
        "\tconst auto i = std::lower_bound(begin, end, name, [](const Message &a, QLatin1StringView b) {",
        "\t\treturn QLatin1StringView(a.name) < b;",
        "\t});",
        "\treturn (i != end && QLatin1StringView(i->name) == name) ? &*i : nullptr;",
        "}",
        "",
        "const Method *FindMethod(QLatin1StringView name) {",
        "\tconst auto begin = std::begin(kMethods);",
        "\tconst auto end = std::end(kMethods);",
        "\tconst auto i = std::lower_bound(begin, end, name, [](const Method &a, QLatin1StringView b) {",
        "\t\treturn QLatin1StringView(a.name) < b;",
        "\t});",
        "\treturn (i != end && QLatin1StringView(i->name) == name) ? &*i : nullptr;",
        "}",
        "",
        "} // namespace Rt::Schema",
        "",
    ]
    DEST.mkdir(parents=True, exist_ok=True)
    (DEST / "rt_schema_auto.cpp").write_text("\n".join(out), encoding="utf-8", newline="\n")
    header = [
        "// Generated by codegen/scripts/gen_rtschema.py from Resources/proto/*.proto - do not edit.",
        "#pragma once",
        "",
        '#include "rt/rt_schema.h"',
        "",
        "namespace Rt::Schema {",
        "",
        f"inline constexpr auto kGeneratedMessages = {len(names)};",
        f"inline constexpr auto kGeneratedMethods = {count};",
        "",
        "} // namespace Rt::Schema",
        "",
    ]
    (DEST / "rt_schema_auto.h").write_text("\n".join(header), encoding="utf-8", newline="\n")
    print("wrote", DEST / "rt_schema_auto.cpp", len(names), "messages,", count, "methods")


if __name__ == "__main__":
    main()
