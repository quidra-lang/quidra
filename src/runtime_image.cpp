#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <algorithm>
#include <cctype>
#include <csetjmp>
#include <cmath>
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
#include <type_traits>
#include <utility>
#include <vector>
#include <webp/decode.h>
#include <webp/encode.h>

extern "C" char* quidra_runtime_copy_text(
    const char* data, unsigned long long size);
extern "C" void* quidra_tensor_from_chw(
    const void* data, int dtype,
    unsigned long long channels,
    unsigned long long height,
    unsigned long long width);
extern "C" int quidra_tensor_chw_info(
    void* raw,
    unsigned long long* channels,
    unsigned long long* height,
    unsigned long long* width);
extern "C" bool quidra_tensor_chw_copy(
    void* raw, void* output, unsigned long long count);

namespace {

// Canonical tensor dtype codes shared with runtime.cpp / llvm_backend.cpp.
constexpr int dtype_int64 = 1;
constexpr int dtype_int8 = 2;
constexpr int dtype_int16 = 3;
constexpr int dtype_int32 = 4;
constexpr int dtype_uint8 = 5;
constexpr int dtype_uint16 = 6;
constexpr int dtype_uint32 = 7;
constexpr int dtype_uint64 = 8;
constexpr int dtype_float64 = 9;
constexpr int dtype_float32 = 10;

std::size_t dtype_bytes(int dtype) {
    switch (dtype) {
        case dtype_int64: return 8;
        case dtype_int8: return 1;
        case dtype_int16: return 2;
        case dtype_int32: return 4;
        case dtype_uint8: return 1;
        case dtype_uint16: return 2;
        case dtype_uint32: return 4;
        case dtype_uint64: return 8;
        case dtype_float64: return 8;
        case dtype_float32: return 4;
        default: throw std::invalid_argument("unsupported image dtype");
    }
}

struct Image {
    int dtype{dtype_uint8};
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

std::size_t sample_count(std::size_t channels, std::size_t height, std::size_t width) {
    return checked_product(checked_product(channels, height, "image size overflow"),
                           width, "image size overflow");
}

std::size_t image_bytes(const Image& image) {
    return checked_product(sample_count(image.channels, image.height, image.width),
                           dtype_bytes(image.dtype), "image byte size overflow");
}

void validate_image_shape(const Image& image) {
    (void)dtype_bytes(image.dtype);
    if (image.channels != 1 && image.channels != 3 && image.channels != 4) {
        throw std::invalid_argument("image channels must be 1, 3, or 4");
    }
    if (image.chw.size() != image_bytes(image)) {
        throw std::invalid_argument("image storage does not match its CHW shape and dtype");
    }
}

bool host_little_endian() {
    const std::uint16_t value = 1;
    return *reinterpret_cast<const std::uint8_t*>(&value) == 1;
}

std::string extension_lower(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

Image hwc_to_chw(const std::uint8_t* source, std::size_t height,
                 std::size_t width, std::size_t channels, int dtype) {
    if (channels != 1 && channels != 3 && channels != 4) {
        throw std::invalid_argument("unsupported image channel count");
    }
    const auto stride = dtype_bytes(dtype);
    Image image;
    image.dtype = dtype;
    image.channels = channels;
    image.height = height;
    image.width = width;
    image.chw.resize(checked_product(sample_count(channels, height, width), stride,
                                     "image byte size overflow"));
    if (!image.chw.empty() && !source) throw std::invalid_argument("null image data");
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const auto destination = ((channel * height + y) * width + x) * stride;
                const auto input = ((y * width + x) * channels + channel) * stride;
                std::memcpy(image.chw.data() + destination, source + input, stride);
            }
        }
    }
    return image;
}

std::vector<std::uint8_t> chw_to_hwc(const Image& image) {
    validate_image_shape(image);
    const auto stride = dtype_bytes(image.dtype);
    std::vector<std::uint8_t> output(image.chw.size());
    for (std::size_t y = 0; y < image.height; ++y) {
        for (std::size_t x = 0; x < image.width; ++x) {
            for (std::size_t channel = 0; channel < image.channels; ++channel) {
                const auto source = ((channel * image.height + y) * image.width + x) * stride;
                const auto destination = ((y * image.width + x) * image.channels + channel) * stride;
                std::memcpy(output.data() + destination, image.chw.data() + source, stride);
            }
        }
    }
    return output;
}

Image tensor_to_image(void* raw, int expected_dtype) {
    unsigned long long channels = 0;
    unsigned long long height = 0;
    unsigned long long width = 0;
    const int dtype = quidra_tensor_chw_info(raw, &channels, &height, &width);
    if (dtype == 0 || (expected_dtype != 0 && dtype != expected_dtype)) {
        throw std::invalid_argument(
            "image.write requires a numeric CHW tensor with shape [1|3|4, H, W]");
    }
    if (channels > std::numeric_limits<std::size_t>::max() ||
        height > std::numeric_limits<std::size_t>::max() ||
        width > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("image shape exceeds addressable storage");
    }
    Image image;
    image.dtype = dtype;
    image.channels = static_cast<std::size_t>(channels);
    image.height = static_cast<std::size_t>(height);
    image.width = static_cast<std::size_t>(width);
    image.chw.resize(image_bytes(image));
    const auto count = sample_count(image.channels, image.height, image.width);
    if (!quidra_tensor_chw_copy(raw, image.chw.data(),
                                static_cast<unsigned long long>(count))) {
        throw std::invalid_argument(
            "image.write requires every image tensor element to be initialized");
    }
    return image;
}

void* image_to_tensor(const Image& image) {
    validate_image_shape(image);
    return quidra_tensor_from_chw(
        image.chw.data(), image.dtype,
        static_cast<unsigned long long>(image.channels),
        static_cast<unsigned long long>(image.height),
        static_cast<unsigned long long>(image.width));
}


template <typename T>
T read_sample(const std::uint8_t* data) {
    T value{};
    std::memcpy(&value, data, sizeof(T));
    return value;
}

template <typename T>
void write_sample(std::uint8_t* data, T value) {
    std::memcpy(data, &value, sizeof(T));
}

template <typename T>
T grayscale_sample(T red, T green, T blue) {
    const double value =
        0.299 * static_cast<double>(red) +
        0.587 * static_cast<double>(green) +
        0.114 * static_cast<double>(blue);
    if constexpr (std::is_integral_v<T>) {
        const double rounded = std::round(value);
        const double low = static_cast<double>(std::numeric_limits<T>::lowest());
        const double high = static_cast<double>(std::numeric_limits<T>::max());
        return static_cast<T>(std::clamp(rounded, low, high));
    } else {
        return static_cast<T>(value);
    }
}

template <typename T>
T opaque_alpha() {
    if constexpr (std::is_integral_v<T>) {
        return std::numeric_limits<T>::max();
    } else {
        return static_cast<T>(1);
    }
}

template <typename T>
Image convert_channels_t(const Image& source, std::size_t target_channels) {
    if (source.channels == target_channels) return source;
    Image output;
    output.dtype = source.dtype;
    output.channels = target_channels;
    output.height = source.height;
    output.width = source.width;
    output.chw.resize(image_bytes(output));
    const auto pixels = checked_product(source.height, source.width, "image size overflow");
    const auto source_at = [&](std::size_t channel, std::size_t index) {
        return read_sample<T>(
            source.chw.data() + (channel * pixels + index) * sizeof(T));
    };
    const auto write_at = [&](std::size_t channel, std::size_t index, T value) {
        write_sample<T>(
            output.chw.data() + (channel * pixels + index) * sizeof(T), value);
    };
    for (std::size_t index = 0; index < pixels; ++index) {
        if (target_channels == 1) {
            if (source.channels == 1) {
                write_at(0, index, source_at(0, index));
            } else {
                write_at(0, index, grayscale_sample(
                    source_at(0, index), source_at(1, index), source_at(2, index)));
            }
            continue;
        }
        if (source.channels == 1) {
            const auto gray = source_at(0, index);
            write_at(0, index, gray);
            write_at(1, index, gray);
            write_at(2, index, gray);
        } else {
            write_at(0, index, source_at(0, index));
            write_at(1, index, source_at(1, index));
            write_at(2, index, source_at(2, index));
        }
        if (target_channels == 4) {
            write_at(3, index,
                     source.channels == 4 ? source_at(3, index) : opaque_alpha<T>());
        }
    }
    return output;
}

Image convert_channels(const Image& source, int target_channels) {
    if (target_channels == 0 || static_cast<std::size_t>(target_channels) == source.channels) {
        return source;
    }
    if (target_channels != 1 && target_channels != 3 && target_channels != 4) {
        throw std::invalid_argument("image channels must be 1, 3, or 4");
    }
    switch (source.dtype) {
        case dtype_int8: return convert_channels_t<std::int8_t>(source, target_channels);
        case dtype_int16: return convert_channels_t<std::int16_t>(source, target_channels);
        case dtype_int32: return convert_channels_t<std::int32_t>(source, target_channels);
        case dtype_int64: return convert_channels_t<std::int64_t>(source, target_channels);
        case dtype_uint8: return convert_channels_t<std::uint8_t>(source, target_channels);
        case dtype_uint16: return convert_channels_t<std::uint16_t>(source, target_channels);
        case dtype_uint32: return convert_channels_t<std::uint32_t>(source, target_channels);
        case dtype_uint64: return convert_channels_t<std::uint64_t>(source, target_channels);
        case dtype_float32: return convert_channels_t<float>(source, target_channels);
        case dtype_float64: return convert_channels_t<double>(source, target_channels);
        default: throw std::invalid_argument("unsupported image dtype");
    }
}

template <typename Source, typename Target>
Image convert_dtype_t(const Image& source, int target_dtype) {
    Image output;
    output.dtype = target_dtype;
    output.channels = source.channels;
    output.height = source.height;
    output.width = source.width;
    output.chw.resize(image_bytes(output));
    const auto count = sample_count(source.channels, source.height, source.width);
    for (std::size_t index = 0; index < count; ++index) {
        const auto value =
            read_sample<Source>(source.chw.data() + index * sizeof(Source));
        Target converted{};
        if constexpr (std::is_floating_point_v<Source> &&
                      std::is_integral_v<Target>) {
            throw std::invalid_argument(
                "image dtype conversion from floating point to integer requires an explicit rounding operation");
        } else if constexpr (std::is_integral_v<Source> &&
                             std::is_integral_v<Target>) {
            if (!std::in_range<Target>(value)) {
                throw std::range_error(
                    "image dtype conversion would change an integer value");
            }
            converted = static_cast<Target>(value);
        } else {
            // Integer-to-float and float-to-float conversion explicitly permits
            // the destination IEEE-754 rounding required by its representation.
            converted = static_cast<Target>(value);
        }
        write_sample<Target>(
            output.chw.data() + index * sizeof(Target), converted);
    }
    return output;
}

template <typename Source>
Image convert_dtype_from(const Image& source, int target_dtype) {
    switch (target_dtype) {
        case dtype_int8: return convert_dtype_t<Source, std::int8_t>(source, target_dtype);
        case dtype_int16: return convert_dtype_t<Source, std::int16_t>(source, target_dtype);
        case dtype_int32: return convert_dtype_t<Source, std::int32_t>(source, target_dtype);
        case dtype_int64: return convert_dtype_t<Source, std::int64_t>(source, target_dtype);
        case dtype_uint8: return convert_dtype_t<Source, std::uint8_t>(source, target_dtype);
        case dtype_uint16: return convert_dtype_t<Source, std::uint16_t>(source, target_dtype);
        case dtype_uint32: return convert_dtype_t<Source, std::uint32_t>(source, target_dtype);
        case dtype_uint64: return convert_dtype_t<Source, std::uint64_t>(source, target_dtype);
        case dtype_float32: return convert_dtype_t<Source, float>(source, target_dtype);
        case dtype_float64: return convert_dtype_t<Source, double>(source, target_dtype);
        default: throw std::invalid_argument("unsupported image target dtype");
    }
}

Image convert_dtype(const Image& source, int target_dtype) {
    if (target_dtype == 0 || source.dtype == target_dtype) return source;
    switch (source.dtype) {
        case dtype_int8: return convert_dtype_from<std::int8_t>(source, target_dtype);
        case dtype_int16: return convert_dtype_from<std::int16_t>(source, target_dtype);
        case dtype_int32: return convert_dtype_from<std::int32_t>(source, target_dtype);
        case dtype_int64: return convert_dtype_from<std::int64_t>(source, target_dtype);
        case dtype_uint8: return convert_dtype_from<std::uint8_t>(source, target_dtype);
        case dtype_uint16: return convert_dtype_from<std::uint16_t>(source, target_dtype);
        case dtype_uint32: return convert_dtype_from<std::uint32_t>(source, target_dtype);
        case dtype_uint64: return convert_dtype_from<std::uint64_t>(source, target_dtype);
        case dtype_float32: return convert_dtype_from<float>(source, target_dtype);
        case dtype_float64: return convert_dtype_from<double>(source, target_dtype);
        default: throw std::invalid_argument("unsupported image source dtype");
    }
}

void require_image_shape(const Image& image,
                         long long channels,
                         long long height,
                         long long width) {
    const auto require_extent = [](std::size_t actual, long long expected,
                                   const char* axis) {
        if (expected < 0) return;
        if (static_cast<unsigned long long>(expected) !=
            static_cast<unsigned long long>(actual)) {
            throw std::invalid_argument(
                std::string("image ") + axis +
                " does not match the expected tensor shape constraint");
        }
    };
    require_extent(image.channels, channels, "channels");
    require_extent(image.height, height, "height");
    require_extent(image.width, width, "width");
}

struct PngError {
    std::jmp_buf jump{};
    char message[256]{};
};

void png_error_callback(png_structp png, png_const_charp message) {
    auto* error = static_cast<PngError*>(png_get_error_ptr(png));
    if (error) {
        std::snprintf(error->message, sizeof(error->message), "%s",
                      message ? message : "PNG operation failed");
        std::longjmp(error->jump, 1);
    }
    std::abort();
}

void png_warning_callback(png_structp, png_const_charp) {}

FILE* image_open_file(const std::string& path, const char* mode) {
#ifdef _WIN32
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), mode) != 0) return nullptr;
    return file;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

Image read_png(const std::string& path) {
    FILE* file = image_open_file(path, "rb");
    if (!file) throw std::runtime_error("cannot open PNG: " + path);

    PngError error{};
    png_structp png = png_create_read_struct(
        PNG_LIBPNG_VER_STRING, &error, png_error_callback, png_warning_callback);
    if (!png) {
        std::fclose(file);
        throw std::bad_alloc();
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(file);
        throw std::bad_alloc();
    }
    std::uint8_t* raw = nullptr;
    png_bytep* rows = nullptr;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4611)
#endif
    if (setjmp(error.jump) != 0) {
        std::free(rows);
        std::free(raw);
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(file);
        throw std::runtime_error(
            std::string("PNG decode failed: ") +
            (error.message[0] ? error.message : path));
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    png_init_io(png, file);
    png_read_info(png, info);
    int color = png_get_color_type(png, info);
    int bits = png_get_bit_depth(png, info);
    const bool has_trns = png_get_valid(png, info, PNG_INFO_tRNS) != 0;

    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && bits < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (has_trns) png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY_ALPHA ||
        (color == PNG_COLOR_TYPE_GRAY && has_trns)) {
        png_set_gray_to_rgb(png);
    }
    if (bits == 16 && host_little_endian()) png_set_swap(png);
    (void)png_set_interlace_handling(png);
    png_read_update_info(png, info);

    const auto width = static_cast<std::size_t>(png_get_image_width(png, info));
    const auto height = static_cast<std::size_t>(png_get_image_height(png, info));
    const int output_bits = png_get_bit_depth(png, info);
    const auto channels = static_cast<std::size_t>(png_get_channels(png, info));
    const int dtype = output_bits == 8 ? dtype_uint8 :
                      output_bits == 16 ? dtype_uint16 : 0;
    if (dtype == 0 || (channels != 1 && channels != 3 && channels != 4)) {
        png_error(png, "unsupported PNG sample layout");
    }
    const auto row_bytes = png_get_rowbytes(png, info);
    const auto expected_row = checked_product(
        checked_product(width, channels, "PNG row size overflow"),
        dtype_bytes(dtype), "PNG row size overflow");
    if (row_bytes != expected_row) png_error(png, "unexpected PNG row layout");
    const auto bytes = checked_product(row_bytes, height, "PNG image size overflow");
    raw = static_cast<std::uint8_t*>(std::malloc(bytes == 0 ? 1 : bytes));
    rows = static_cast<png_bytep*>(std::malloc(
        (height == 0 ? 1 : height) * sizeof(png_bytep)));
    if (!raw || !rows) png_error(png, "out of memory");
    for (std::size_t y = 0; y < height; ++y) rows[y] = raw + y * row_bytes;
    png_read_image(png, rows);
    png_read_end(png, nullptr);

    auto image = hwc_to_chw(raw, height, width, channels, dtype);
    std::free(rows);
    std::free(raw);
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(file);
    return image;
}

void write_png(const std::string& path, const Image& image) {
    validate_image_shape(image);
    if (image.dtype != dtype_uint8 && image.dtype != dtype_uint16) {
        throw std::invalid_argument("PNG can represent only uint8 or uint16 image tensors");
    }
    if (image.width > std::numeric_limits<png_uint_32>::max() ||
        image.height > std::numeric_limits<png_uint_32>::max()) {
        throw std::overflow_error("PNG dimensions exceed codec limits");
    }
    FILE* file = image_open_file(path, "wb");
    if (!file) throw std::runtime_error("cannot open PNG for writing: " + path);
    auto raw = chw_to_hwc(image);

    PngError error{};
    png_structp png = png_create_write_struct(
        PNG_LIBPNG_VER_STRING, &error, png_error_callback, png_warning_callback);
    if (!png) {
        std::fclose(file);
        throw std::bad_alloc();
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        std::fclose(file);
        throw std::bad_alloc();
    }
    png_bytep* rows = nullptr;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4611)
#endif
    if (setjmp(error.jump) != 0) {
        std::free(rows);
        png_destroy_write_struct(&png, &info);
        std::fclose(file);
        throw std::runtime_error(
            std::string("PNG encode failed: ") +
            (error.message[0] ? error.message : path));
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    png_init_io(png, file);
    const int color = image.channels == 1 ? PNG_COLOR_TYPE_GRAY :
                      image.channels == 3 ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_RGBA;
    const int bits = image.dtype == dtype_uint16 ? 16 : 8;
    png_set_IHDR(png, info,
                 static_cast<png_uint_32>(image.width),
                 static_cast<png_uint_32>(image.height),
                 bits, color, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    if (bits == 16 && host_little_endian()) png_set_swap(png);
    const auto row_bytes = checked_product(
        checked_product(image.width, image.channels, "PNG row size overflow"),
        dtype_bytes(image.dtype), "PNG row size overflow");
    rows = static_cast<png_bytep*>(std::malloc(
        (image.height == 0 ? 1 : image.height) * sizeof(png_bytep)));
    if (!rows) png_error(png, "out of memory");
    for (std::size_t y = 0; y < image.height; ++y) rows[y] = raw.data() + y * row_bytes;
    png_write_image(png, rows);
    png_write_end(png, nullptr);
    std::free(rows);
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
}

struct JpegError {
    jpeg_error_mgr manager{};
    std::jmp_buf* jump{};
    char message[JMSG_LENGTH_MAX]{};
};

void jpeg_error_exit(j_common_ptr common) {
    auto* error = reinterpret_cast<JpegError*>(common->err);
    (*common->err->format_message)(common, error->message);
    std::longjmp(*error->jump, 1);
}

Image read_jpeg(const std::string& path) {
    FILE* file = image_open_file(path, "rb");
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
        throw std::runtime_error(std::string("JPEG decode failed: ") +
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
    const auto count = sample_count(channels, height, width);
    raw = static_cast<unsigned char*>(std::malloc(count == 0 ? 1 : count));
    if (!raw) {
        jpeg_destroy_decompress(&info);
        created = false;
        std::fclose(file);
        throw std::bad_alloc();
    }
    while (info.output_scanline < info.output_height) {
        JSAMPROW row = raw + static_cast<std::size_t>(info.output_scanline) * width * channels;
        jpeg_read_scanlines(&info, &row, 1);
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    created = false;
    std::fclose(file);
    auto image = hwc_to_chw(raw, height, width, channels, dtype_uint8);
    std::free(raw);
    return image;
}

void write_jpeg(const std::string& path, const Image& image, int quality) {
    validate_image_shape(image);
    if (image.dtype != dtype_uint8) {
        throw std::invalid_argument("JPEG can represent only uint8 image tensors");
    }
    if (image.channels == 4) {
        throw std::invalid_argument("JPEG cannot preserve alpha; convert RGBA explicitly before writing");
    }
    if (quality < 1 || quality > 100) {
        throw std::invalid_argument("JPEG quality must be between 1 and 100");
    }
    if (image.width > std::numeric_limits<JDIMENSION>::max() ||
        image.height > std::numeric_limits<JDIMENSION>::max()) {
        throw std::overflow_error("JPEG dimensions exceed codec limits");
    }
    auto raw = chw_to_hwc(image);
    FILE* file = image_open_file(path, "wb");
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
        throw std::runtime_error(std::string("JPEG encode failed: ") +
                                 (error.message[0] ? error.message : path));
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    jpeg_create_compress(&info);
    created = true;
    jpeg_stdio_dest(&info, file);
    info.image_width = static_cast<JDIMENSION>(image.width);
    info.image_height = static_cast<JDIMENSION>(image.height);
    info.input_components = static_cast<int>(image.channels);
    info.in_color_space = image.channels == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, quality, TRUE);
    jpeg_start_compress(&info, TRUE);
    while (info.next_scanline < info.image_height) {
        JSAMPROW row = raw.data() + static_cast<std::size_t>(info.next_scanline) *
                                      image.width * image.channels;
        jpeg_write_scanlines(&info, &row, 1);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);
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
    const auto height = static_cast<std::size_t>(signed_height > 0 ? signed_height : -signed_height);
    const auto channels = static_cast<std::size_t>(info.bits / 8);
    const auto row_raw = checked_product(width, channels, "BMP row size overflow");
    if (row_raw > std::numeric_limits<std::size_t>::max() - 3) {
        throw std::overflow_error("BMP row size overflow");
    }
    const auto row = ((row_raw + 3) / 4) * 4;
    std::vector<std::uint8_t> encoded(checked_product(row, height, "BMP image size overflow"));
    input.seekg(file_header.offset);
    input.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if (!input) throw std::runtime_error("truncated BMP");
    std::vector<std::uint8_t> rgb(sample_count(channels, height, width));
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
    return hwc_to_chw(rgb.data(), height, width, channels, dtype_uint8);
}

void write_bmp(const std::string& path, const Image& image) {
    validate_image_shape(image);
    if (image.dtype != dtype_uint8) {
        throw std::invalid_argument("BMP can represent only uint8 image tensors in the supported layout");
    }
    if (image.channels != 3 && image.channels != 4) {
        throw std::invalid_argument("BMP writer requires RGB or RGBA");
    }
    if (image.width > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
        image.height > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::overflow_error("BMP dimensions exceed codec limits");
    }
    const auto row_raw = checked_product(image.width, image.channels, "BMP row size overflow");
    if (row_raw > std::numeric_limits<std::size_t>::max() - 3) {
        throw std::overflow_error("BMP row size overflow");
    }
    const auto row = ((row_raw + 3) / 4) * 4;
    const auto bytes = checked_product(row, image.height, "BMP image size overflow");
    const auto header_bytes = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    if (bytes > std::numeric_limits<std::uint32_t>::max() - header_bytes) {
        throw std::overflow_error("BMP file exceeds codec limits");
    }
    BmpFileHeader file_header{0x4D42, static_cast<std::uint32_t>(header_bytes + bytes),
                              0, 0, static_cast<std::uint32_t>(header_bytes)};
    BmpInfoHeader info{40, static_cast<std::int32_t>(image.width),
                       static_cast<std::int32_t>(image.height), 1,
                       static_cast<std::uint16_t>(image.channels * 8), 0,
                       static_cast<std::uint32_t>(bytes), 2835, 2835, 0, 0};
    std::vector<std::uint8_t> encoded(bytes, 0);
    auto raw = chw_to_hwc(image);
    for (std::size_t y = 0; y < image.height; ++y) {
        const auto destination_y = image.height - 1 - y;
        for (std::size_t x = 0; x < image.width; ++x) {
            const auto source = (y * image.width + x) * image.channels;
            const auto destination = destination_y * row + x * image.channels;
            encoded[destination] = raw[source + 2];
            encoded[destination + 1] = raw[source + 1];
            encoded[destination + 2] = raw[source];
            if (image.channels == 4) encoded[destination + 3] = raw[source + 3];
        }
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open BMP for writing: " + path);
    output.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if (!output) throw std::runtime_error("BMP encode failed: " + path);
}

Image read_webp(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open WebP: " + path);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
    if (bytes.empty()) throw std::runtime_error("empty WebP file");
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(bytes.data(), bytes.size(), &features) != VP8_STATUS_OK ||
        features.width <= 0 || features.height <= 0) {
        throw std::runtime_error("invalid WebP");
    }
    int width = 0;
    int height = 0;
    const std::size_t channels = features.has_alpha ? 4 : 3;
    std::uint8_t* decoded = features.has_alpha
        ? WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height)
        : WebPDecodeRGB(bytes.data(), bytes.size(), &width, &height);
    if (!decoded || width <= 0 || height <= 0) {
        if (decoded) WebPFree(decoded);
        throw std::runtime_error("WebP decode failed");
    }
    auto image = hwc_to_chw(decoded, static_cast<std::size_t>(height),
                            static_cast<std::size_t>(width), channels, dtype_uint8);
    WebPFree(decoded);
    return image;
}

void write_webp(const std::string& path, const Image& image, int quality) {
    validate_image_shape(image);
    if (image.dtype != dtype_uint8) {
        throw std::invalid_argument("WebP can represent only uint8 image tensors through this codec");
    }
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
        ? WebPEncodeRGBA(raw.data(), width, height, stride, static_cast<float>(quality), &encoded)
        : WebPEncodeRGB(raw.data(), width, height, stride, static_cast<float>(quality), &encoded);
    if (size == 0 || !encoded) throw std::runtime_error("WebP encode failed");
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        WebPFree(encoded);
        throw std::runtime_error("cannot open WebP for writing: " + path);
    }
    output.write(reinterpret_cast<const char*>(encoded), static_cast<std::streamsize>(size));
    WebPFree(encoded);
    if (!output) throw std::runtime_error("WebP encode failed: " + path);
}

int tiff_dtype(std::uint16_t sample_format, std::uint16_t bits) {
    if (sample_format == SAMPLEFORMAT_UINT) {
        if (bits == 8) return dtype_uint8;
        if (bits == 16) return dtype_uint16;
        if (bits == 32) return dtype_uint32;
        if (bits == 64) return dtype_uint64;
    } else if (sample_format == SAMPLEFORMAT_INT) {
        if (bits == 8) return dtype_int8;
        if (bits == 16) return dtype_int16;
        if (bits == 32) return dtype_int32;
        if (bits == 64) return dtype_int64;
    } else if (sample_format == SAMPLEFORMAT_IEEEFP) {
        if (bits == 32) return dtype_float32;
        if (bits == 64) return dtype_float64;
    }
    return 0;
}

std::pair<std::uint16_t, std::uint16_t> tiff_sample_description(int dtype) {
    switch (dtype) {
        case dtype_uint8: return {SAMPLEFORMAT_UINT, 8};
        case dtype_uint16: return {SAMPLEFORMAT_UINT, 16};
        case dtype_uint32: return {SAMPLEFORMAT_UINT, 32};
        case dtype_uint64: return {SAMPLEFORMAT_UINT, 64};
        case dtype_int8: return {SAMPLEFORMAT_INT, 8};
        case dtype_int16: return {SAMPLEFORMAT_INT, 16};
        case dtype_int32: return {SAMPLEFORMAT_INT, 32};
        case dtype_int64: return {SAMPLEFORMAT_INT, 64};
        case dtype_float32: return {SAMPLEFORMAT_IEEEFP, 32};
        case dtype_float64: return {SAMPLEFORMAT_IEEEFP, 64};
        default: throw std::invalid_argument("unsupported TIFF dtype");
    }
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
    std::uint16_t sample_format = SAMPLEFORMAT_UINT;
    TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_PHOTOMETRIC, &photometric);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sample_format);
    const bool gray = samples == 1 && photometric == PHOTOMETRIC_MINISBLACK;
    const bool rgb = (samples == 3 || samples == 4) && photometric == PHOTOMETRIC_RGB;
    const int dtype = tiff_dtype(sample_format, bits);
    if (width == 0 || height == 0 || dtype == 0 || planar != PLANARCONFIG_CONTIG ||
        (!gray && !rgb)) {
        TIFFClose(tiff);
        throw std::runtime_error(
            "TIFF decoder requires contiguous grayscale/RGB/RGBA with an exact Quidra numeric sample dtype");
    }
    const auto channels = static_cast<std::size_t>(samples);
    const auto row_bytes = checked_product(
        checked_product(static_cast<std::size_t>(width), channels, "TIFF row size overflow"),
        dtype_bytes(dtype), "TIFF row size overflow");
    const auto codec_row = TIFFScanlineSize(tiff);
    if (codec_row < 0 || static_cast<std::size_t>(codec_row) < row_bytes) {
        TIFFClose(tiff);
        throw std::runtime_error("invalid TIFF scanline size");
    }
    std::vector<std::uint8_t> scanline(static_cast<std::size_t>(codec_row));
    std::vector<std::uint8_t> raw(checked_product(row_bytes, height, "TIFF image size overflow"));
    for (std::uint32_t y = 0; y < height; ++y) {
        if (TIFFReadScanline(tiff, scanline.data(), y, 0) < 0) {
            TIFFClose(tiff);
            throw std::runtime_error("TIFF decode failed");
        }
        std::memcpy(raw.data() + static_cast<std::size_t>(y) * row_bytes,
                    scanline.data(), row_bytes);
    }
    TIFFClose(tiff);
    return hwc_to_chw(raw.data(), height, width, channels, dtype);
}

void write_tiff(const std::string& path, const Image& image) {
    validate_image_shape(image);
    if (image.width > std::numeric_limits<std::uint32_t>::max() ||
        image.height > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("TIFF dimensions exceed codec limits");
    }
    const auto [sample_format, bits] = tiff_sample_description(image.dtype);
    TIFF* tiff = TIFFOpen(path.c_str(), "w");
    if (!tiff) throw std::runtime_error("cannot open TIFF for writing: " + path);
    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(image.width));
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(image.height));
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, static_cast<std::uint16_t>(image.channels));
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, bits);
    TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, sample_format);
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
    const auto row_bytes = checked_product(
        checked_product(image.width, image.channels, "TIFF row size overflow"),
        dtype_bytes(image.dtype), "TIFF row size overflow");
    for (std::size_t y = 0; y < image.height; ++y) {
        if (TIFFWriteScanline(tiff, raw.data() + y * row_bytes,
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
    if (extension == ".jpg" || extension == ".jpeg") return write_jpeg(path, image, quality);
    if (extension == ".bmp") return write_bmp(path, image);
    if (extension == ".webp") return write_webp(path, image, quality);
    if (extension == ".tif" || extension == ".tiff") return write_tiff(path, image);
    throw std::invalid_argument("unsupported image extension: " + extension);
}

} // namespace

extern "C" void* quidra_image_read(const char* path,
                                     int expected_dtype,
                                     int target_dtype,
                                     int target_channels,
                                     long long expected_channels,
                                     long long expected_height,
                                     long long expected_width,
                                     int* actual_dtype) {
    image_last_error.clear();
    if (actual_dtype) *actual_dtype = 0;
    try {
        if (!path || !*path) throw std::invalid_argument("image path is empty");
        auto image = read_image(path);
        if (expected_dtype != 0 && image.dtype != expected_dtype) {
            throw std::invalid_argument(
                "image dtype does not match the statically expected tensor dtype");
        }
        image = convert_channels(image, target_channels);
        image = convert_dtype(image, target_dtype);
        require_image_shape(
            image, expected_channels, expected_height, expected_width);
        auto* tensor = image_to_tensor(image);
        if (!tensor) throw std::runtime_error("cannot allocate image tensor");
        if (actual_dtype) *actual_dtype = image.dtype;
        return tensor;
    } catch (const std::exception& error) {
        image_set_error(error.what());
        return nullptr;
    } catch (...) {
        image_set_error("unknown image read failure");
        return nullptr;
    }
}

extern "C" bool quidra_image_write(const char* path, void* tensor,
                                     int expected_dtype, long long quality) {
    image_last_error.clear();
    try {
        if (!path || !*path) throw std::invalid_argument("image path is empty");
        if (quality < 1 || quality > 100) {
            throw std::invalid_argument("image quality must be between 1 and 100");
        }
        const auto image = tensor_to_image(tensor, expected_dtype);
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
