#!/usr/bin/env bash
set -euo pipefail

quidra="$1"
if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg CLI not available; skipping video runtime test"
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
video="$tmp/input.mp4"
ffmpeg -loglevel error -y \
  -f lavfi -i "color=c=red:s=8x6:r=2:d=1.5" \
  -frames:v 3 -c:v mpeg4 -pix_fmt yuv420p "$video"

cat > "$tmp/video.qui" <<QUI
auto opened = video.open("$video")
match opened
    video.Reader reader
        print(reader.width())
        print(reader.height())
        print(reader.position())

        tensor<uint8><3, 6, 8> | none | error first = reader.read()
        match first
            tensor<uint8><3, 6, 8> pixels
                int[] shape = pixels.shape()
                print(shape[0])
                print(shape[1])
                print(shape[2])
            none
                print("unexpected-eof")
            error problem
                print(problem)
        print(reader.position())

        auto sought = reader.seek(0)
        match sought
            void
                print(reader.position())
            error problem
                print(problem)

        tensor<float32><1, 6, 8> | none | error gray = reader.read(channel = 1, type = float32)
        match gray
            tensor<float32><1, 6, 8> pixels
                int[] shape = pixels.shape()
                print(shape[0])
                print(shape[1])
                print(shape[2])
            none
                print("unexpected-eof")
            error problem
                print(problem)
        print(reader.position())

        video.Reader copied = reader
        tensor<uint8><3, 6, 8> | none | error copied_frame = copied.read()
        match copied_frame
            tensor<uint8><3, 6, 8> pixels
                print(copied.position())
            none
                print("unexpected-eof")
            error problem
                print(problem)
        print(reader.position())
    error problem
        print(problem)
QUI

actual="$("$quidra" run "$tmp/video.qui")"
expected=$'8\n6\n0\n3\n6\n8\n1\n0\n1\n6\n8\n1\n2\n1'
if [[ "$actual" != "$expected" ]]; then
  printf 'video output mismatch\nexpected:\n%s\nactual:\n%s\n' "$expected" "$actual" >&2
  exit 1
fi

cat > "$tmp/invalid.qui" <<'QUI'
void inspect(video.Reader reader)
    reader.read(channel = 2)
QUI
if "$quidra" check "$tmp/invalid.qui" >/dev/null 2>&1; then
  echo "video.Reader.read accepted invalid channel" >&2
  exit 1
fi
