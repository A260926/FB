#!/bin/sh
echo "TMPDIR=[$TMPDIR]"
mkdir -p "$TMPDIR/test-write" && echo "write-ok"
