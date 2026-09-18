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

cat > "$TMP/tiff-dtypes.qui" <<QUI
tensor<int8> i8 = tensor.zeros<int8>([1, 1, 1])
i8[0, 0, 0] = int8(-8)
auto i8_written = image.write("$TMP/i8.tiff", i8)
match i8_written
    void
        tensor<int8> | error loaded = image.read("$TMP/i8.tiff")
        match loaded
            tensor<int8> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

tensor<int16> i16 = tensor.zeros<int16>([1, 1, 1])
i16[0, 0, 0] = int16(-1600)
auto i16_written = image.write("$TMP/i16.tiff", i16)
match i16_written
    void
        tensor<int16> | error loaded = image.read("$TMP/i16.tiff")
        match loaded
            tensor<int16> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

tensor<int32> i32 = tensor.zeros<int32>([1, 1, 1])
i32[0, 0, 0] = int32(-320000)
auto i32_written = image.write("$TMP/i32.tiff", i32)
match i32_written
    void
        tensor<int32> | error loaded = image.read("$TMP/i32.tiff")
        match loaded
            tensor<int32> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

tensor<int> i64 = tensor.zeros<int>([1, 1, 1])
i64[0, 0, 0] = -640000
auto i64_written = image.write("$TMP/i64.tiff", i64)
match i64_written
    void
        tensor<int> | error loaded = image.read("$TMP/i64.tiff")
        match loaded
            tensor<int> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

tensor<uint8> u8 = tensor.zeros<uint8>([1, 1, 1])
u8[0, 0, 0] = uint8(8)
auto u8_written = image.write("$TMP/u8.tiff", u8)
match u8_written
    void
        tensor<uint8> | error loaded = image.read("$TMP/u8.tiff")
        match loaded
            tensor<uint8> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

tensor<uint16> u16 = tensor.zeros<uint16>([1, 1, 1])
u16[0, 0, 0] = uint16(1600)
auto u16_written = image.write("$TMP/u16.tiff", u16)
match u16_written
    void
        tensor<uint16> | error loaded = image.read("$TMP/u16.tiff")
        match loaded
            tensor<uint16> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

tensor<uint32> u32 = tensor.zeros<uint32>([1, 1, 1])
u32[0, 0, 0] = uint32(320000)
auto u32_written = image.write("$TMP/u32.tiff", u32)
match u32_written
    void
        tensor<uint32> | error loaded = image.read("$TMP/u32.tiff")
        match loaded
            tensor<uint32> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

tensor<uint64> u64 = tensor.zeros<uint64>([1, 1, 1])
u64[0, 0, 0] = uint64(640000)
auto u64_written = image.write("$TMP/u64.tiff", u64)
match u64_written
    void
        tensor<uint64> | error loaded = image.read("$TMP/u64.tiff")
        match loaded
            tensor<uint64> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

tensor<float32> f32 = tensor.zeros<float32>([1, 1, 1])
f32[0, 0, 0] = float32(1.5)
auto f32_written = image.write("$TMP/f32.tiff", f32)
match f32_written
    void
        tensor<float32> | error loaded = image.read("$TMP/f32.tiff")
        match loaded
            tensor<float32> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

tensor<float> f64 = tensor.zeros<float>([1, 1, 1])
f64[0, 0, 0] = 2.5
auto f64_written = image.write("$TMP/f64.tiff", f64)
match f64_written
    void
        tensor<float> | error loaded = image.read("$TMP/f64.tiff")
        match loaded
            tensor<float> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

// uint16 PNG writing must preserve the exact 16-bit sample.
tensor<uint16> png16 = tensor.zeros<uint16>([1, 1, 1])
png16[0, 0, 0] = uint16(4660)
auto png16_written = image.write("$TMP/written-u16.png", png16)
match png16_written
    void
        tensor<uint16> | error loaded = image.read("$TMP/written-u16.png")
        match loaded
            tensor<uint16> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

// Explicit channel conversion and source/output shape constraints.
tensor<uint8> rgb = tensor.zeros<uint8>([3, 1, 1])
rgb[0, 0, 0] = uint8(255)
rgb[1, 0, 0] = uint8(0)
rgb[2, 0, 0] = uint8(0)
auto rgb_written = image.write("$TMP/rgb.png", rgb)
match rgb_written
    void
        tensor<uint8><3, _, _> | error exact_rgb = image.read("$TMP/rgb.png")
        match exact_rgb
            tensor<uint8><3, _, _> pixels
                print(pixels.shape()[0])
            error problem
                print(problem)

        tensor<uint8><1, _, _> | error rejected_gray = image.read("$TMP/rgb.png")
        match rejected_gray
            tensor<uint8><1, _, _> pixels
                print("unexpected")
            error problem
                print("shape-error")

        tensor<uint8><1, _, _> | error gray = image.read("$TMP/rgb.png", channels = 1)
        match gray
            tensor<uint8><1, _, _> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)

        tensor<float32><3, _, _> | error float_rgb = image.read("$TMP/rgb.png", dtype = float32)
        match float_rgb
            tensor<float32><3, _, _> pixels
                print(pixels[0, 0, 0].item())
            error problem
                print(problem)
    error problem
        print(problem)

// A target format must reject a dtype it cannot represent instead of narrowing.
auto bad_jpeg = image.write("$TMP/u16.jpg", png16)
match bad_jpeg
    void
        print("converted")
    error problem
        print("rejected")
QUI

tiff_output="$("$QUIDRA" "$TMP/tiff-dtypes.qui")"
tiff_expected="$(printf '%s\n' -8 -1600 -320000 -640000 8 1600 320000 640000 1.5 2.5 4660 3 shape-error 76 255.0 rejected)"
if [[ "$tiff_output" != "$tiff_expected" ]]; then
    echo "unexpected image dtype round-trip output:" >&2
    printf '%s\n' "$tiff_output" >&2
    exit 1
fi

echo "image dtype integration: ok"
