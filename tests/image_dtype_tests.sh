#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP/u16.png" <<'PY'
import binascii
import struct
import sys
import zlib

path = sys.argv[1]

def chunk(kind, payload):
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", binascii.crc32(body) & 0xffffffff)

png = bytearray(b"\x89PNG\r\n\x1a\n")
png += chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 16, 0, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(b"\x00\x12\x34"))
png += chunk(b"IEND", b"")
open(path, "wb").write(png)
PY

cat > "$TMP/narrow-u16.qui" <<QUI
tensor<uint16> | error loaded = image.read("$TMP/u16.png")
match loaded
    tensor<uint16> pixels
        print(pixels.shape()[0])
        print(pixels[0, 0, 0].item())
    error problem
        print(problem)
QUI
narrow_output="$("$QUIDRA" "$TMP/narrow-u16.qui")"
[[ "$narrow_output" == "$(printf '1\n4660')" ]]

cat > "$TMP/narrow-mismatch.qui" <<QUI
tensor<uint8> | error loaded = image.read("$TMP/u16.png")
match loaded
    tensor<uint8> pixels
        print("converted")
    error problem
        print("dtype-error")
QUI
[[ "$("$QUIDRA" "$TMP/narrow-mismatch.qui")" == "dtype-error" ]]

cat > "$TMP/auto-u16.qui" <<QUI
auto loaded = image.read("$TMP/u16.png")
match loaded
    tensor<int8> pixels
        print("int8")
    tensor<int16> pixels
        print("int16")
    tensor<int32> pixels
        print("int32")
    tensor<int> pixels
        print("int64")
    tensor<uint8> pixels
        print("uint8")
    tensor<uint16> pixels
        print("uint16")
        print(pixels[0, 0, 0].item())
    tensor<uint32> pixels
        print("uint32")
    tensor<uint64> pixels
        print("uint64")
    tensor<float32> pixels
        print("float32")
    tensor<float> pixels
        print("float64")
    error problem
        print("error")
QUI
auto_output="$("$QUIDRA" "$TMP/auto-u16.qui")"
[[ "$auto_output" == "$(printf 'uint16\n4660')" ]]

cat > "$TMP/try-u16.qui" <<QUI
tensor<uint16> | error load_u16(string path)
    tensor<uint16> pixels = try image.read(path)
    return pixels

auto loaded = load_u16("$TMP/u16.png")
match loaded
    tensor<uint16> pixels
        print(pixels[0, 0, 0].item())
    error problem
        print(problem)
QUI
[[ "$("$QUIDRA" "$TMP/try-u16.qui")" == "4660" ]]

cat > "$TMP/no-implicit-error-drop.qui" <<QUI
tensor<uint8> pixels = image.read("$TMP/u16.png")
print(pixels.shape()[0])
QUI
set +e
"$QUIDRA" check "$TMP/no-implicit-error-drop.qui" --json >"$TMP/no-implicit-error-drop.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'TYPE_MISMATCH' "$TMP/no-implicit-error-drop.json"

echo "image dtype integration: ok"
