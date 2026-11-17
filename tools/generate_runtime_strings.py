#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

"""Generate the immutable C++ fallback from Pulse's two runtime STRINGTABLEs."""

from pathlib import Path
import re
import sys


LANGUAGES = {"0x04, 0x02": "zh", "0x09, 0x01": "en"}
ESCAPES = {"\\": "\\", "n": "\n", "r": "\r", "t": "\t", "a": "\a",
           "b": "\b", "f": "\f", "v": "\v"}
ENTRY = re.compile(r'(IDS_[A-Z0-9_]+)\s+"((?:[^"\\]|""|\\.)*)"')


def decode_rc(value):
    result = []
    index = 0
    while index < len(value):
        char = value[index]
        if char == '"':
            if value[index:index + 2] != '""':
                raise ValueError("a literal RC quote must be doubled")
            index += 1
        elif char == "\\":
            index += 1
            if index == len(value) or value[index] not in ESCAPES:
                raise ValueError("unsupported RC escape")
            char = ESCAPES[value[index]]
        result.append(char)
        index += 1
    return "".join(result)


def parse_rc(source):
    tables = {}
    language = None
    state = "language"
    for number, raw in enumerate(source.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("//") or line == '#include "string_ids.h"':
            continue
        try:
            if state == "language" and line.startswith("LANGUAGE "):
                language = LANGUAGES.get(line.removeprefix("LANGUAGE "))
                if language is None or language in tables:
                    raise ValueError("unsupported or duplicate language")
                tables[language] = {}
                state = "table"
            elif state == "table" and line == "STRINGTABLE":
                state = "begin"
            elif state == "begin" and line == "BEGIN":
                state = "entries"
            elif state == "entries" and line == "END":
                state = "language"
            elif state == "entries" and (match := ENTRY.fullmatch(line)):
                identifier, value = match.groups()
                if identifier in tables[language]:
                    raise ValueError(f"duplicate ID {identifier}")
                tables[language][identifier] = decode_rc(value)
            else:
                raise ValueError("invalid runtime resource line")
        except ValueError as error:
            raise ValueError(f"line {number}: {error}") from error
    if state != "language" or set(tables) != {"zh", "en"}:
        raise ValueError("expected complete Chinese and English STRINGTABLEs")
    if not tables["zh"] or tables["zh"].keys() != tables["en"].keys():
        raise ValueError("Chinese and English tables must have the same nonempty IDs")
    return tables


def cpp_literal(value):
    escapes = {value: "\\" + key for key, value in ESCAPES.items()}
    escapes['"'] = '\\"'
    return 'L"' + "".join(escapes.get(char, char) for char in value) + '"'


def generate(source):
    tables = parse_rc(source)
    lines = ["// Generated from runtime_messages.rc; do not edit.",
             "const std::wstring& RuntimeMessage(UINT id, Language language) {",
             "    switch (id) {"]
    for identifier, chinese in tables["zh"].items():
        lines.extend([f"    case {identifier}: {{",
                      f"        static const std::wstring chinese = {cpp_literal(chinese)};",
                      f"        static const std::wstring english = {cpp_literal(tables['en'][identifier])};",
                      "        return language == Language::ZhCN ? chinese : english;",
                      "    }"])
    lines.extend(["    default: return g_empty;", "    }", "}", ""])
    return "\n".join(lines)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: generate_runtime_strings.py input.rc output.h")
    try:
        output = generate(Path(sys.argv[1]).read_text(encoding="utf-8-sig"))
        destination = Path(sys.argv[2])
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(output, encoding="utf-8", newline="\n")
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    main()
