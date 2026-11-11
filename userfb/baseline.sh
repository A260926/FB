#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT HUP INT TERM
cc -std=c11 -O1 -DUNBOUND_OBJECTS -I src/core -I src/raytracer -I lib/SimplexNoise userfb/baseline.c src/core/*.c src/raytracer/material.c src/raytracer/object.c src/raytracer/accel.c src/raytracer/render.c lib/SimplexNoise/SimplexNoise.c -lm -o "$b/baseline"
"$b/baseline"
