#!/usr/bin/env python3
"""Regenerate the mqtt.c / mqtt.h blocks of the amalgamated mongoose files
from src/mqtt.c and src/mqtt.h, preserving the surrounding layout exactly.

Only the mqtt sections are touched; everything else in mongoose.c/h is copied
byte for byte from the existing files. The transform reproduces the layout
found in the original amalgamation: local #include "..." lines are dropped
(leaving a blank line in the body), and the header section is separated from
its neighbours by exactly five blank lines on each side.
"""
import sys


def strip_local_includes(text):
    out = []
    for l in text.split("\n"):
        # Both `#pragma once` and local includes are dropped by the
        # amalgamation, each leaving a blank line behind.
        if l.startswith('#include "') or l == "#pragma once":
            out.append("")
        else:
            out.append(l)
    return "\n".join(out)


def find_define(lines, name, start=0):
    for i in range(start, len(lines)):
        l = lines[i]
        if l == name or l.startswith(name + " "):
            return i
    raise SystemExit("marker not found: " + name)


def body_range(src_lines):
    """First non-blank through last non-blank line of the source file."""
    s = next(i for i, l in enumerate(src_lines) if l.strip())
    e = max(i for i, l in enumerate(src_lines) if l.strip())
    return s, e


def splice_c(mongoose_c, new_mqtt_c):
    lines = mongoose_c.split("\n")
    start = find_define(lines, "#define MQTT_CLEAN_SESSION")
    nxt = next(i for i, l in enumerate(lines) if l.startswith('#line 1 "src/net.c"'))
    assert lines[nxt - 1].startswith("#ifdef MG_ENABLE_LINES"), lines[nxt - 1]
    end = nxt - 2  # inclusive; blank separator lines live after 'end'
    while lines[end] == "":
        end -= 1
    src = strip_local_includes(new_mqtt_c).split("\n")
    s, e = body_range(src)
    # Rebuild: original leading blanks + new body + original trailing blanks
    lead = lines[start - 1:start]
    out = lines[:start] + src[s:e + 1] + lines[end + 1:]
    return "\n".join(out), (start, end)


def splice_h(mongoose_h, new_mqtt_h):
    lines = mongoose_h.split("\n")
    start = find_define(lines, "#define MQTT_CMD_CONNECT")
    decl = next(i for i, l in enumerate(lines) if l.strip() == "size_t ofs);" and i > start)
    src = strip_local_includes(new_mqtt_h).split("\n")
    s, e = body_range(src)
    # The surrounding blank lines already live in the file before `start`
    # and after `decl`; only the source body itself is spliced in.
    out = lines[:start] + src[s:e + 1] + lines[decl + 1:]
    return "\n".join(out), (start, decl)


def read_exact(path):
    # newline='' disables universal-newline translation so CRLF survives.
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_exact(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def main():
    root = sys.argv[1]
    new_c = read_exact(root + "/src/mqtt.c")
    new_h = read_exact(root + "/src/mqtt.h")
    c_text, c_span = splice_c(read_exact(root + "/mongoose.c"), new_c)
    h_text, h_span = splice_h(read_exact(root + "/mongoose.h"), new_h)
    write_exact(root + "/mongoose.c", c_text)
    write_exact(root + "/mongoose.h", h_text)
    print("c span", c_span, "h span", h_span)


if __name__ == "__main__":
    main()
