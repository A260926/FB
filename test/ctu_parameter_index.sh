#!/usr/bin/env bash
# CTU parameter index analysis: multi-file command line test.
#
# Scans the samples in test/ctu_parameter_index both with a plain single
# process multi-file scan and with --cppcheck-build-dir (analysis information
# is saved and loaded back). Both entry points must report the same
# ctuArrayIndex diagnostics: id, severity, buffer size in bytes, accessed
# byte offset and the call chain locations.
#
# usage: ctu_parameter_index.sh <path to cppcheck executable>
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <cppcheck executable>" >&2
    exit 1
fi
cppcheck="$1"
samples="$(cd "$(dirname "$0")" && pwd)/ctu_parameter_index"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail() {
    echo "CTU_PARAMETER_INDEX_XML_FAIL: $*" >&2
    exit 1
}

scan() { # $1: sample name, $2: mode (plain|builddir)
    local s="$1" mode="$2"
    if [ "$mode" = plain ]; then
        "$cppcheck" --platform=unix64 --check-level=exhaustive --max-ctu-depth=5 --xml \
            "$samples/$s" >"$work/$s.$mode.out" 2>"$work/$s.$mode.xml"
    else
        mkdir -p "$work/bd-$s"
        "$cppcheck" --platform=unix64 --check-level=exhaustive --max-ctu-depth=5 \
            --cppcheck-build-dir="$work/bd-$s" --xml \
            "$samples/$s" >"$work/$s.$mode.out" 2>"$work/$s.$mode.xml"
        [ -s "$work/bd-$s/files.txt" ] || fail "$s: cppcheck build dir was not used"
    fi
    xmllint --noout "$work/$s.$mode.xml" 2>/dev/null || fail "$s ($mode): invalid XML output"
}

error_count() { # $1: xml file
    xmllint --xpath "count(/results/errors/error[@id='ctuArrayIndex'])" "$1"
}

expect_no_error() { # $1: sample, $2: mode
    local count
    count=$(error_count "$work/$1.$2.xml")
    [ "$count" = 0 ] || fail "$1 ($2): expected no ctuArrayIndex, got $count"
}

expect_error() { # $1: sample, $2: mode, $3: cwe, $4: message, $5: space separated file:line:col list
    local s="$1" mode="$2" cwe="$3" msg="$4" locs="$5"
    local xml="$work/$s.$mode.xml"
    local e="/results/errors/error[@id='ctuArrayIndex']"
    local count
    count=$(error_count "$xml")
    [ "$count" = 1 ] || fail "$s ($mode): expected 1 ctuArrayIndex, got $count"
    [ "$(xmllint --xpath "string($e/@severity)" "$xml")" = "error" ] || fail "$s ($mode): severity is not error"
    [ "$(xmllint --xpath "string($e/@cwe)" "$xml")" = "$cwe" ] || fail "$s ($mode): expected cwe $cwe"
    xmllint --xpath "string($e/@msg)" "$xml" | grep -Fq "$msg" || fail "$s ($mode): message does not contain '$msg'"
    local want=0 got i
    for _ in $locs; do want=$((want + 1)); done
    got=$(xmllint --xpath "count($e/location)" "$xml")
    [ "$got" = "$want" ] || fail "$s ($mode): expected $want call chain locations, got $got"
    i=0
    for loc in $locs; do
        i=$((i + 1))
        local file="${loc%%:*}" rest line col
        rest="${loc#*:}"
        line="${rest%%:*}"
        col="${rest##*:}"
        [ "$(xmllint --xpath "string($e/location[$i]/@file)" "$xml")" = "$samples/$s/$file" ] || \
            fail "$s ($mode): location $i is not $file"
        [ "$(xmllint --xpath "string($e/location[$i]/@line)" "$xml")" = "$line" ] || \
            fail "$s ($mode): location $i ($file) is not at line $line"
        [ "$(xmllint --xpath "string($e/location[$i]/@column)" "$xml")" = "$col" ] || \
            fail "$s ($mode): location $i ($file) is not at column $col"
    done
}

for mode in plain builddir; do
    for s in overflow paired negative swap unknown; do
        scan "$s" "$mode"
    done

    # multi-level forwarding with a constant added at the relay: leaf index is 3+1=4
    expect_error overflow "$mode" 788 \
        "buffer size is 16 and it is accessed at offset 16." \
        "sink.c:2:28 relay.c:2:33 entry.c:2:41"

    # safe pairing: the small buffer must not be combined with the index of the other call
    expect_no_error paired "$mode"

    # negative subscript after p[i-1] with i==0
    expect_error negative "$mode" 786 \
        "buffer size is 16 and it is accessed at offset -4." \
        "sink.c:2:28 entry.c:2:40"

    # swapped argument slots with add and subtract at the relays: leaf index is 3+2-1=4
    expect_error swap "$mode" 788 \
        "buffer size is 16 and it is accessed at offset 16." \
        "sink.c:2:28 middle.c:2:34 relay.c:2:35 entry.c:2:41"

    # unknown integer input: no definite error
    expect_no_error unknown "$mode"
done

# both entry points must give the same verdicts
for s in overflow paired negative swap unknown; do
    diff "$work/$s.plain.xml" "$work/$s.builddir.xml" >/dev/null || \
        fail "$s: plain scan and cppcheck-build-dir scan give different results"
done

echo "CTU_PARAMETER_INDEX_XML_OK"
