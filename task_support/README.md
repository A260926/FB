The support C sources are original CC0-1.0 public input/build helpers.

baseline.sh compiles the portable C path (no OMXDL/NEON/ARM assembly). No Ruby,
Emscripten, FFmpeg, packages, device, or download is used by these commands.
fixture.c writes two I_PCM reference pictures (POC 0 and 4), optionally followed
in decoding order by a non-reference B picture (POC 2), using B_Bi_16x16,
zero MVDs, zero residuals, unweighted prediction and disabled deblocking.
The pixel recipe is public in task_pixel(). It is an input example, not a
complete test of B decoding. baseline.sh observe reports the old B rejection;
its exit status is not a target-success assertion.

target.sh compiles the future target_cases.c with the actual decoder, runs
three groups and checks their completion outside the process. check.h describes
the entry point. The actual B assertions are intentionally absent. Keep this
driver and the original I_PCM baseline checks when adding decoder support.

The upstream test/*.h264 files remain intact. Upstream test/Rakefile can download
movie images; it is not called here. The upstream posix test program can be built
directly from src/*.c without Ruby. See the upstream README and LICENSE.md for
the original decoder, wrapper, and Big Buck Bunny sample origins.
