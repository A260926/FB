#!/bin/sh
#
# Build the ORIGINAL VisualOn AAC encoder (from the pristine HEAD tree) and
# demonstrate what the legacy VO_PID_AAC_ENCPARAM re-configuration does to a
# continuous stream: it reopens the core and resets the bit reservoir, the
# fractional frame-length phase and the threshold-adaptation history, even
# when the same bit rate is requested.
#
# Produces 96 ADTS frames and prints the internal state immediately before
# and after the legacy re-set so the reset is visible.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/devcheck/build/baseline"
PRISTINE="$WORK/pristine"

rm -rf "$WORK"
mkdir -p "$PRISTINE"

# Original, unmodified sources/headers.
git -C "$ROOT" archive HEAD | tar -x -C "$PRISTINE"

INC="-I$PRISTINE/aacenc/inc -I$PRISTINE/aacenc/basic_op -I$PRISTINE/common/include"
CFLAGS="-std=c99 -O2 -D__unused= -Wno-misleading-indentation"

SRCS="
$PRISTINE/common/cmnMemory.c
$PRISTINE/aacenc/basic_op/basicop2.c
$PRISTINE/aacenc/basic_op/oper_32b.c
$PRISTINE/aacenc/src/aac_rom.c
$PRISTINE/aacenc/src/aacenc.c
$PRISTINE/aacenc/src/aacenc_core.c
$PRISTINE/aacenc/src/adj_thr.c
$PRISTINE/aacenc/src/band_nrg.c
$PRISTINE/aacenc/src/bit_cnt.c
$PRISTINE/aacenc/src/bitbuffer.c
$PRISTINE/aacenc/src/bitenc.c
$PRISTINE/aacenc/src/block_switch.c
$PRISTINE/aacenc/src/channel_map.c
$PRISTINE/aacenc/src/dyn_bits.c
$PRISTINE/aacenc/src/grp_data.c
$PRISTINE/aacenc/src/interface.c
$PRISTINE/aacenc/src/line_pe.c
$PRISTINE/aacenc/src/memalign.c
$PRISTINE/aacenc/src/ms_stereo.c
$PRISTINE/aacenc/src/pre_echo_control.c
$PRISTINE/aacenc/src/psy_configuration.c
$PRISTINE/aacenc/src/psy_main.c
$PRISTINE/aacenc/src/qc_main.c
$PRISTINE/aacenc/src/quantize.c
$PRISTINE/aacenc/src/sf_estim.c
$PRISTINE/aacenc/src/spreading.c
$PRISTINE/aacenc/src/stat_bits.c
$PRISTINE/aacenc/src/tns.c
$PRISTINE/aacenc/src/transform.c
"

echo "== compiling original library =="
cc $CFLAGS $INC $SRCS -c
for o in *.o; do mv "$o" "$WORK/"; done
ar rcs "$WORK/libvo-aacenc-pristine.a" "$WORK"/*.o
rm -f *.o

# The demo only uses legacy fields, so it builds against the pristine headers.
echo "== building legacy re-configuration demo =="
cc $CFLAGS $INC "$ROOT/devcheck/baseline_demo.c" \
    "$WORK/libvo-aacenc-pristine.a" -o "$WORK/baseline_demo"

echo "== running demo (96 ADTS frames) =="
"$WORK/baseline_demo" "$WORK/baseline.aac"

echo "== output =="
ls -l "$WORK/baseline.aac"
python3 -I -c "
import sys
d=open('$WORK/baseline.aac','rb').read(); off=0; n=0
while off < len(d):
    assert d[off]==0xFF and (d[off+1]&0xF0)==0xF0
    fl=((d[off+3]&3)<<11)|(d[off+4]<<3)|((d[off+5]>>5)&7)
    off+=fl; n+=1
assert off==len(d)
print('ADTS frames:', n)
"
