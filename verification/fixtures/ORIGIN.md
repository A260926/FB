These two 96x80 single-frame IVF files are generated test patterns, not third-party media. Generated locally using FFmpeg testsrc2 -> YUV4MPEG and AOMedia aomenc 3.15.1. No encoder is needed to run baseline or target tests. Reproduction (bash, requires those upstream tools):

for spec in '420-8 yuv420p 8' '420-10 yuv420p10le 10'; do
  read -r name pix depth <<< "$spec"
  ffmpeg -hide_banner -loglevel error -f lavfi -i 'testsrc2=size=96x80:rate=1' -frames:v 1 -pix_fmt "$pix" -strict -1 -f yuv4mpegpipe - | aomenc --codec=av1 --ivf --passes=1 --cpu-used=8 --cq-level=40 --limit=1 --bit-depth="$depth" --input-bit-depth="$depth" --profile=0 --output="$name.ivf" -
done

Use embedded files as inputs; bytewise encoder reproducibility is not assumed. These generated test fixtures may be used under the repository BSD-2-Clause license.
