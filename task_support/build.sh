#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build
if [ ! -f .build/Makefile ]; then
 (cd .build; ../configure --disable-shared --enable-static --disable-openmp --disable-formats --disable-devices --without-libltdl --without-ladspa --without-magic --without-png --without-id3tag --without-libgsm --without-opencore-amrnb --without-opencore-amrwb --without-vo-amrwbenc --without-mad --without-lame --without-twolame --without-libsndfile CC=cc CFLAGS="-O2 -std=gnu99")
fi
make -C .build -j2
make -C .build/src sox_sample_test
cc -std=c99 -O2 task_support/pcm_probe.c -o .build/pcm_probe
