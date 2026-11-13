#!/bin/sh
#
# Build the modified VisualOn AAC encoder together with tests/live_bitrate.c
# and run the five live bit-rate test groups:
#   phase, history, reservoir, streaming, compatibility.
# Also builds and runs the standalone live-bit-rate example.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/devcheck/build/target"

rm -rf "$WORK"
mkdir -p "$WORK"

INC="-I$ROOT/aacenc/inc -I$ROOT/aacenc/basic_op -I$ROOT/common/include"
CFLAGS="-std=c99 -O2 -D__unused= -Wno-misleading-indentation"

SRCS="
$ROOT/common/cmnMemory.c
$ROOT/aacenc/basic_op/basicop2.c
$ROOT/aacenc/basic_op/oper_32b.c
$ROOT/aacenc/src/aac_rom.c
$ROOT/aacenc/src/aacenc.c
$ROOT/aacenc/src/aacenc_core.c
$ROOT/aacenc/src/adj_thr.c
$ROOT/aacenc/src/band_nrg.c
$ROOT/aacenc/src/bit_cnt.c
$ROOT/aacenc/src/bitbuffer.c
$ROOT/aacenc/src/bitenc.c
$ROOT/aacenc/src/block_switch.c
$ROOT/aacenc/src/channel_map.c
$ROOT/aacenc/src/dyn_bits.c
$ROOT/aacenc/src/grp_data.c
$ROOT/aacenc/src/interface.c
$ROOT/aacenc/src/line_pe.c
$ROOT/aacenc/src/memalign.c
$ROOT/aacenc/src/ms_stereo.c
$ROOT/aacenc/src/pre_echo_control.c
$ROOT/aacenc/src/psy_configuration.c
$ROOT/aacenc/src/psy_main.c
$ROOT/aacenc/src/qc_main.c
$ROOT/aacenc/src/quantize.c
$ROOT/aacenc/src/sf_estim.c
$ROOT/aacenc/src/spreading.c
$ROOT/aacenc/src/stat_bits.c
$ROOT/aacenc/src/tns.c
$ROOT/aacenc/src/transform.c
"

echo "== compiling modified library =="
(cd "$WORK" && cc $CFLAGS $INC $SRCS -c)
ar rcs "$WORK/libvo-aacenc.a" "$WORK"/*.o

echo "== building live bit-rate test =="
cc $CFLAGS $INC "$ROOT/tests/live_bitrate.c" "$WORK/libvo-aacenc.a" \
   -o "$WORK/live_bitrate"

echo "== building live bit-rate example =="
cc $CFLAGS $INC "$ROOT/examples/live_bitrate_example.c" "$WORK/libvo-aacenc.a" \
   -o "$WORK/live_bitrate_example"

echo "== running tests =="
"$WORK/live_bitrate"

echo "== running example =="
"$WORK/live_bitrate_example" "$WORK/live_example.aac"
python3 -I -c "
d=open('$WORK/live_example.aac','rb').read(); off=0; n=0
while off < len(d):
    assert d[off]==0xFF and (d[off+1]&0xF0)==0xF0
    fl=((d[off+3]&3)<<11)|(d[off+4]<<3)|((d[off+5]>>5)&7)
    off+=fl; n+=1
assert off==len(d)
print('ADTS frames in example output:', n)
"
