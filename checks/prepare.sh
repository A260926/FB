#!/bin/bash
set -euo pipefail
make -s -f checks/Makefile -j2 .checks-build/liblnlib.a
