#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <algorithm>
#include <cctype>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <jpeglib.h>
#include <limits>
#include <new>
#include <png.h>
#include <stdexcept>
#include <string>
#include <tiffio.h>
#include <vector>
#include <webp/decode.h>
#include <webp/encode.h>

extern "C" void* quidra_managed_alloc(unsigned long long bytes);
extern "C" char* quidra_runtime_copy_text(
    const char* data, unsigned long long size);
extern "C" void* quidra_tensor_from_u8_chw(
    const unsigned char* data,
    unsigned long long channels,
    unsigned long long height,
    unsigned long long width);
extern "C" bool quidra_tensor_u8_chw_info(
    void* raw,
    unsigned long long* channels,
    unsigned long long* height,
    unsigned long long* width);
extern "C" bool quidra_tensor_u8_chw_copy(
    void* raw,
    unsigned char* output,
    unsigned long long count);

namespace {

struct Image {
    std::size_t channels{};
    std::size_t height{};
    std::size_t width{};
    std::vector<std::uint8_t> chw;
};

thread_local std::string image_last_error;

void image_set_error(const std::string& message) {
    image_last_error = message.empty() ? "image operation failed" : message;
}

char* image_copy_string(const std::string& value) {
    return quidra_runtime_copy_text(
        value.data(), static_cast<unsigned long long>(value.size()));
}

std::size_t checked_product(std::size_t a, std::size_t b, const char* what) {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        throw std::overflow_error(what);
    }
    return a * b;
}

std::size_t image_count(std::size_t channels, std::size_t height, std::size_t width) {
    return checked_product(checked_product(channels, height, "image size overflow"),
                           width, "image size overflow");
}

void validate_image_shape(const Image& image) {
    if (image.channels != 1 && image.channels != 3 && image.channels != 4) {
        throw std::invalid_argument("image channels must be 1, 3, or 4");
    }
    if (image.chw.size() != image_count(image.channels, image.height, image.width)) {
        throw std::invalid_argument("image storage does not match its CHW shape");
    }
}

std::string extension_lower(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

Image hwc_to_chw(const std::uint8_t* source, std::size_t height,
                 std::size_t width, std::size_t channels) {
    if (channels != 1 && channels != 3 && channels != 4) {
        throw std::invalid_argument("unsupported image channel count");
    }
    Image image;
    image.channels = channels;
    image.height = height;
    image.width = width;
    const auto count = image_count(channels, height, width);
    image.chw.resize(count);
    if (count != 0 && !source) throw std::invalid_argument("null image data");
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                image.chw[(channel * height + y) * width + x] =
                    source[(y * width + x) * channels + channel];
            }
        }
    }
    return image;
}

std::vector<std::uint8_t> chw_to_hwc(const Image& image) {
    validate_image_shape(image);
    std::vector<std::uint8_t> output(image.chw.size());
    for (std::size_t y = 0; y < image.height; ++y) {
        for (std::size_t x = 0; x < image.width; ++x) {
            for (std::size_t channel = 0; channel < image.channels; ++channel) {
                output[(y * image.width + x) * image.channels + channel] =
                    image.chw[(channel * image.height + y) * image.width + x];
            }
        }
    }
    return output;
}

Image tensor_to_image(void* raw) {
    unsigned long long channels = 0;
    unsigned long long height = 0;
    unsigned long long width = 0;
    if (!quidra_tensor_u8_chw_info(raw, &channels, &height, &width)) {
        throw std::invalid_argument(
            "image.write requires tensor<uint8> with CHW shape [1|3|4, H, W]");
    }
    if (channels > std::numeric_limits<std::size_t>::max() ||
        height > std::numeric_limits<std::size_t>::max() ||
        width > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("image shape exceeds addressable storage");
    }
    Image image;
    image.channels = static_cast<std::size_t>(channels);
    image.height = static_cast<std::size_t>(height);
    image.width = static_cast<std::size_t>(width);
    image.chw.resize(image_count(image.channels, image.height, image.width));
    if (!quidra_tensor_u8_chw_copy(
            raw, image.chw.data(),
            static_cast<unsigned long long>(image.chw.size()))) {
        throw std::invalid_argument(
            "image.write requires every image tensor element to be initialized");
    }
    return image;
}

void* image_to_tensor(const Image& image) {
    validate_image_shape(image);
    return quidra_tensor_from_u8_chw(
        image.chw.data(),
        static_cast<unsigned long long>(image.channels),
        static_cast<unsigned long long>(image.height),
        static_cast<unsigned long long>(image.width));
}

Image read_png(const std::string& path) {
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&png, path.c_str())) {
        throw std::runtime_error(
            std::string("PNG decode failed: ") +
            (png.message[0] ? png.message : path));
    }

    const bool color = (png.format & PNG_FORMAT_FLAG_COLOR) != 0;
    const bool alpha = (png.format & PNG_FORMAT_FLAG_ALPHA) != 0;
    if (!color && !alpha) png.format = PNG_FORMAT_GRAY;
    else if (color && !alpha) png.format = PNG_FORMAT_RGB;
    else png.format = PNG_FORMAT_RGBA;

    const auto bytes = PNG_IMAGE_SIZE(png);
    std::vector<std::uint8_t> raw(bytes);
    if (!png_image_finish_read(&png, nullptr, raw.data(), 0, nullptr)) {
        const std::string message =
            std::string("PNG decode failed: ") +
            (png.message[0] ? png.message : path);
        png_image_free(&png);
        throw std::runtime_error(message);
    }
    const auto channels =
        png.format == PNG_FORMAT_GRAY ? std::size_t{1} :
        png.format == PNG_FORMAT_RGB ? std::size_t{3} : std::size_t{4};
    auto result = hwc_to_chw(raw.data(), png.height, png.width, channels);
    png_image_free(&png);
    return result;
}

void write_png(const std::string& path, const Image& image) {
    validate_image_shape(image);
    auto raw = chw_to_hwc(image);
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (image.width > std::numeric_limits<png_uint_32>::max() ||
        image.height > std::numeric_limits<png_uint_32>::max()) {
        throw std::overflow_error("PNG dimensions exceed codec limits");
    }
    png.width = static_cast<png_uint_32>(image.width);
    png.height = static_cast<png_uint_32>(image.height);
    png.format =
        image.channels == 1 ? PNG_FORMAT_GRAY :
        image.channels == 3 ? PNG_FORMAT_RGB : PNG_FORMAT_RGBA;
    if (!png_image_write_to_file(
            &png, path.c_str(), 0, raw.data(), 0, nullptr)) {
        throw std::runtime_error(
            std::string("PNG encode failed: ") +
            (png.message[0] ? png.message : path));
    }
}

struct JpegError {
    jpeg_error_mgr manager{};
    std::jmp_buf* jump{};
    char message[JMSG_LENGTH_MAX]{};
};

FILE* jpeg_open_file(const std::string& path, const char* mode) {
#ifdef _WIN32
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), mode) != 0) return nullptr;
    return file;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

void jpeg_error_exit(j_common_ptr common) {
    auto* error = reinterpret_cast<JpegError*>(common->err);
    (*common->err->format_message)(common, error->message);
    std::longjmp(*error->jump, 1);
}

Image read_jpeg(const std::string& path) {
    FILE* file = jpeg_open_file(path, "rb");
    if (!file) throw std::runtime_error("cannot open JPEG: " + path);

    jpeg_decompress_struct info{};
    std::jmp_buf jump{};
    JpegError error{};
    error.jump = &jump;
    info.err = jpeg_std_error(&error.manager);
    error.manager.error_exit = jpeg_error_exit;
    bool created = false;
    unsigned char* raw = nullptr;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4611)
#endif
    if (setjmp(jump) != 0) {
        if (raw) std::free(raw);
        if (created) jpeg_destroy_decompress(&info);
        std::fclose(file);
        throw std::runtime_error(
            std::string("JPEG decode failed: ") +
            (error.message[0] ? error.message : path));
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    jpeg_create_decompress(&info);
    created = true;
    jpeg_stdio_src(&info, file);
    jpeg_read_header(&info, TRUE);
    info.out_color_space =
        info.jpeg_color_space == JCS_GRAYSCALE ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_start_decompress(&info);

    const auto width = static_cast<std::size_t>(info.output_width);
    const auto height = static_cast<std::size_t>(info.output_height);
    const auto channels = static_cast<std::size_t>(info.output_components);
    const auto count = image_count(channels, height, width);
    raw = static_cast<unsigned char*>(std::malloc(count == 0 ? 1 : count));
    if (!raw) {
        jpeg_destroy_decompress(&info);
        created = false;
        std::fclose(file);
        throw std::bad_alloc();
    }

    while (info.output_scanline < info.output_height) {
        JSAMPROW row =
            raw + static_cast<std::size_t>(info.output_scanline) *
                      width * channels;
        jpeg_read_scanlines(&info, &row, 1);
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    created = false;
    std::fclose(file);

    auto image = hwc_to_chw(raw, height, width, channels);
    std::free(raw);
    return image;
}

void write_jpeg(const std::string& path, const Image& image, int quality) {
    validate_image_shape(image);
    if (image.channels == 4) {
        throw std::invalid_argument(
            "JPEG cannot preserve alpha; convert RGBA explicitly before writing");
    }
    if (quality < 1 || quality > 100) {
        throw std::invalid_argument("JPEG quality must be between 1 and 100");
    }
    auto raw = chw_to_hwc(image);
    FILE* file = jpeg_open_file(path, "wb");
    if (!file) throw std::runtime_error("cannot open JPEG for writing: " + path);

    jpeg_compress_struct info{};
    std::jmp_buf jump{};
    JpegError error{};
    error.jump = &jump;
    info.err = jpeg_std_error(&error.manager);
    error.manager.error_exit = jpeg_error_exit;
    bool created = false;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4611)
#endif
    if (setjmp(jump) != 0) {
        if (created) jpeg_destroy_compress(&info);
        std::fclose(file);
        throw std::runtime_error(
            std::string("JPEG encode failed: ") +
            (error.message[0] ? error.message : path));
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    jpeg_create_compress(&info);
    created = true;
    jpeg_stdio_dest(&info, file);
    if (image.width > std::numeric_limits<JDIMENSION>::max() ||
        image.height > std::numeric_limits<JDIMENSION>::max()) {
        jpeg_destroy_compress(&info);
        created = false;
        std::fclose(file);
        throw std::overflow_error("JPEG dimensions exceed codec limits");
    }
    info.image_width = static_cast<JDIMENSION>(image.width);
    info.image_height = static_cast<JDIMENSION>(image.height);
    info.input_components = static_cast<int>(image.channels);
    info.in_color_space = image.channels == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, quality, TRUE);
    jpeg_start_compress(&info, TRUE);
    while (info.next_scanline < info.image_height) {
        JSAMPROW row =
            raw.data() + static_cast<std::size_t>(info.next_scanline) *
                             image.width * image.channels;
        jpeg_write_scanlines(&info, &row, 1);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);
    created = false;
    std::fclose(file);
}

#pragma pack(push, 1)
struct BmpFileHeader {
    std::uint16_t type;
    std::uint32_t size;
    std::uint16_t reserved1;
    std::uint16_t reserved2;
    std::uint32_t offset;
};
struct BmpInfoHeader {
    std::uint32_t size;
    std::int32_t width;
    std::int32_t height;
    std::uint16_t planes;
    std::uint16_t bits;
    std::uint32_t compression;
    std::uint32_t image_size;
    std::int32_t xppm;
    std::int32_t yppm;
    std::uint32_t colors;
    std::uint32_t important;
};
#pragma pack(pop)

Image read_bmp(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open BMP: " + path);
    BmpFileHeader file_header{};
    BmpInfoHeader info{};
    input.read(reinterpret_cast<char*>(&file_header), sizeof(file_header));
    input.read(reinterpret_cast<char*>(&info), sizeof(info));
    if (!input || file_header.type != 0x4D42 || info.size < 40 ||
        info.planes != 1 || info.compression != 0 ||
        (info.bits != 24 && info.bits != 32) ||
        info.width <= 0 || info.height == 0) {
        throw std::runtime_error("unsupported BMP format");
    }
    const auto width = static_cast<std::size_t>(info.width);
    const auto signed_height = static_cast<long long>(info.height);
    const auto height = static_cast<std::size_t>(
        signed_height > 0 ? signed_height : -signed_height);
    const auto channels = static_cast<std::size_t>(info.bits / 8);
    const auto row_raw = checked_product(width, channels, "BMP row size overflow");
    if (row_raw > std::numeric_limits<std::size_t>::max() - 3) {
        throw std::overflow_error("BMP row size overflow");
    }
    const auto row = ((row_raw + 3) / 4) * 4;
    std::vector<std::uint8_t> encoded(
        checked_product(row, height, "BMP image size overflow"));
    input.seekg(file_header.offset);
    input.read(reinterpret_cast<char*>(encoded.data()),
               static_cast<std::streamsize>(encoded.size()));
    if (!input) throw std::runtime_error("truncated BMP");

    std::vector<std::uint8_t> rgb(
        image_count(channels, height, width));
    const bool bottom_up = info.height > 0;
    for (std::size_t y = 0; y < height; ++y) {
        const auto source_y = bottom_up ? height - 1 - y : y;
        for (std::size_t x = 0; x < width; ++x) {
            const auto source = source_y * row + x * channels;
            const auto destination = (y * width + x) * channels;
            rgb[destination] = encoded[source + 2];
            rgb[destination + 1] = encoded[source + 1];
            rgb[destination + 2] = encoded[source];
            if (channels == 4) rgb[destination + 3] = encoded[source + 3];
        }
    }
    return hwc_to_chw(rgb.data(), height, width, channels);
}

void write_bmp(const std::string& path, const Image& image) {
    validate_image_shape(image);
    if (image.channels != 3 && image.channels != 4) {
        throw std::invalid_argument("BMP writer requires RGB or RGBA");
    }
    if (image.width > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
        image.height > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::overflow_error("BMP dimensions exceed codec limits");
    }
    const auto row_raw =
        checked_product(image.width, image.channels, "BMP row size overflow");
    if (row_raw > std::numeric_limits<std::size_t>::max() - 3) {
        throw std::overflow_error("BMP row size overflow");
    }
    const auto row = ((row_raw + 3) / 4) * 4;
    const auto bytes = checked_product(row, image.height, "BMP image size overflow");
    const auto header_bytes = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    if (bytes > std::numeric_limits<std::uint32_t>::max() - header_bytes) {
        throw std::overflow_error("BMP file exceeds codec limits");
    }

    BmpFileHeader file_header{
        0x4D42,
        static_cast<std::uint32_t>(header_bytes + bytes),
        0, 0,
        static_cast<std::uint32_t>(header_bytes)};
    BmpInfoHeader info{
        40,
        static_cast<std::int32_t>(image.width),
        static_cast<std::int32_t>(image.height),
        1,
        static_cast<std::uint16_t>(image.channels * 8),
        0,
        static_cast<std::uint32_t>(bytes),
        2835, 2835, 0, 0};

    std::vector<std::uint8_t> encoded(bytes, 0);
    auto raw = chw_to_hwc(image);
    for (std::size_t y = 0; y < image.height; ++y) {
        const auto destination_y = image.height - 1 - y;
        for (std::size_t x = 0; x < image.width; ++x) {
            const auto source = (y * image.width + x) * image.channels;
            const auto destination =
                destination_y * row + x * image.channels;
            encoded[destination] = raw[source + 2];
            encoded[destination + 1] = raw[source + 1];
            encoded[destination + 2] = raw[source];
            if (image.channels == 4) {
                encoded[destination + 3] = raw[source + 3];
            }
        }
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open BMP for writing: " + path);
    output.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    if (!output) throw std::runtime_error("BMP encode failed: " + path);
}

Image read_webp(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open WebP: " + path);
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (bytes.empty()) throw std::runtime_error("empty WebP file");

    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(bytes.data(), bytes.size(), &features) != VP8_STATUS_OK ||
        features.width <= 0 || features.height <= 0) {
        throw std::runtime_error("invalid WebP");
    }
    int width = 0;
    int height = 0;
    std::uint8_t* decoded = nullptr;
    const std::size_t channels = features.has_alpha ? 4 : 3;
    decoded = features.has_alpha
        ? WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height)
        : WebPDecodeRGB(bytes.data(), bytes.size(), &width, &height);
    if (!decoded || width <= 0 || height <= 0) {
        if (decoded) WebPFree(decoded);
        throw std::runtime_error("WebP decode failed");
    }
    auto image = hwc_to_chw(
        decoded, static_cast<std::size_t>(height),
        static_cast<std::size_t>(width), channels);
    WebPFree(decoded);
    return image;
}

void write_webp(const std::string& path, const Image& image, int quality) {
    validate_image_shape(image);
    if (image.channels != 3 && image.channels != 4) {
        throw std::invalid_argument("WebP writer requires RGB or RGBA");
    }
    if (quality < 1 || quality > 100) {
        throw std::invalid_argument("WebP quality must be between 1 and 100");
    }
    if (image.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        image.height > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        image.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) /
                          image.channels) {
        throw std::overflow_error("WebP dimensions exceed codec limits");
    }

    auto raw = chw_to_hwc(image);
    std::uint8_t* encoded = nullptr;
    const auto width = static_cast<int>(image.width);
    const auto height = static_cast<int>(image.height);
    const auto stride = static_cast<int>(image.width * image.channels);
    const auto size = image.channels == 4
        ? WebPEncodeRGBA(raw.data(), width, height, stride,
                         static_cast<float>(quality), &encoded)
        : WebPEncodeRGB(raw.data(), width, height, stride,
                        static_cast<float>(quality), &encoded);
    if (size == 0 || !encoded) {
        throw std::runtime_error("WebP encode failed");
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        WebPFree(encoded);
        throw std::runtime_error("cannot open WebP for writing: " + path);
    }
    output.write(reinterpret_cast<const char*>(encoded),
                 static_cast<std::streamsize>(size));
    WebPFree(encoded);
    if (!output) throw std::runtime_error("WebP encode failed: " + path);
}

Image read_tiff(const std::string& path) {
    TIFF* tiff = TIFFOpen(path.c_str(), "r");
    if (!tiff) throw std::runtime_error("cannot open TIFF: " + path);

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t samples = 1;
    std::uint16_t bits = 0;
    std::uint16_t planar = PLANARCONFIG_CONTIG;
    std::uint16_t photometric = 0;
    TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_PHOTOMETRIC, &photometric);

    const bool gray = samples == 1 && photometric == PHOTOMETRIC_MINISBLACK;
    const bool rgb = (samples == 3 || samples == 4) && photometric == PHOTOMETRIC_RGB;
    if (width == 0 || height == 0 || bits != 8 ||
        planar != PLANARCONFIG_CONTIG || (!gray && !rgb)) {
        TIFFClose(tiff);
        throw std::runtime_error(
            "TIFF decoder supports only 8-bit contiguous grayscale, RGB, or RGBA");
    }

    const auto channels = static_cast<std::size_t>(samples);
    const auto row_bytes = image_count(channels, 1, width);
    const auto codec_row = TIFFScanlineSize(tiff);
    if (codec_row < 0 || static_cast<std::size_t>(codec_row) < row_bytes) {
        TIFFClose(tiff);
        throw std::runtime_error("invalid TIFF scanline size");
    }

    std::vector<std::uint8_t> scanline(static_cast<std::size_t>(codec_row));
    std::vector<std::uint8_t> raw(image_count(channels, height, width));
    for (std::uint32_t y = 0; y < height; ++y) {
        if (TIFFReadScanline(tiff, scanline.data(), y, 0) < 0) {
            TIFFClose(tiff);
            throw std::runtime_error("TIFF decode failed");
        }
        std::memcpy(raw.data() + static_cast<std::size_t>(y) * row_bytes,
                    scanline.data(), row_bytes);
    }
    TIFFClose(tiff);
    return hwc_to_chw(raw.data(), height, width, channels);
}

void write_tiff(const std::string& path, const Image& image) {
    validate_image_shape(image);
    if (image.width > std::numeric_limits<std::uint32_t>::max() ||
        image.height > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("TIFF dimensions exceed codec limits");
    }
    TIFF* tiff = TIFFOpen(path.c_str(), "w");
    if (!tiff) throw std::runtime_error("cannot open TIFF for writing: " + path);

    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH,
                 static_cast<std::uint32_t>(image.width));
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH,
                 static_cast<std::uint32_t>(image.height));
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL,
                 static_cast<std::uint16_t>(image.channels));
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC,
                 image.channels == 1 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB);
    TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    if (image.channels == 4) {
        std::uint16_t extra = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, 1, &extra);
    }

    auto raw = chw_to_hwc(image);
    const auto row_bytes =
        checked_product(image.width, image.channels, "TIFF row size overflow");
    for (std::size_t y = 0; y < image.height; ++y) {
        if (TIFFWriteScanline(
                tiff, raw.data() + y * row_bytes,
                static_cast<std::uint32_t>(y), 0) < 0) {
            TIFFClose(tiff);
            throw std::runtime_error("TIFF encode failed");
        }
    }
    TIFFClose(tiff);
}

Image read_image(const std::string& path) {
    const auto extension = extension_lower(path);
    if (extension == ".png") return read_png(path);
    if (extension == ".jpg" || extension == ".jpeg") return read_jpeg(path);
    if (extension == ".bmp") return read_bmp(path);
    if (extension == ".webp") return read_webp(path);
    if (extension == ".tif" || extension == ".tiff") return read_tiff(path);
    throw std::invalid_argument("unsupported image extension: " + extension);
}

void write_image(const std::string& path, const Image& image, int quality) {
    const auto extension = extension_lower(path);
    if (extension == ".png") return write_png(path, image);
    if (extension == ".jpg" || extension == ".jpeg") {
        return write_jpeg(path, image, quality);
    }
    if (extension == ".bmp") return write_bmp(path, image);
    if (extension == ".webp") return write_webp(path, image, quality);
    if (extension == ".tif" || extension == ".tiff") {
        return write_tiff(path, image);
    }
    throw std::invalid_argument("unsupported image extension: " + extension);
}

} // namespace

extern "C" void* quidra_image_read_u8(const char* path) {
    image_last_error.clear();
    try {
        if (!path || !*path) throw std::invalid_argument("image path is empty");
        auto image = read_image(path);
        auto* tensor = image_to_tensor(image);
        if (!tensor) throw std::runtime_error("cannot allocate image tensor");
        return tensor;
    } catch (const std::exception& error) {
        image_set_error(error.what());
        return nullptr;
    } catch (...) {
        image_set_error("unknown image read failure");
        return nullptr;
    }
}

extern "C" bool quidra_image_write_u8(const char* path, void* tensor, long long quality) {
    image_last_error.clear();
    try {
        if (!path || !*path) throw std::invalid_argument("image path is empty");
        if (quality < 1 || quality > 100) {
            throw std::invalid_argument("image quality must be between 1 and 100");
        }
        const auto image = tensor_to_image(tensor);
        write_image(path, image, static_cast<int>(quality));
        return true;
    } catch (const std::exception& error) {
        image_set_error(error.what());
        return false;
    } catch (...) {
        image_set_error("unknown image write failure");
        return false;
    }
}

extern "C" char* quidra_image_last_error_copy() {
    return image_copy_string(
        image_last_error.empty() ? "image operation failed" : image_last_error);
}
