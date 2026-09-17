from pathlib import Path

path = Path("src/runtime_image.cpp")
text = path.read_text()


def once(old: str, new: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected exactly one match ({count}) for:\n{old}")
    text = text.replace(old, new, 1)


once(
    '''    if (image.dtype != dtype_uint8 && image.dtype != dtype_uint16) {
        throw std::invalid_argument("PNG can represent only uint8 or uint16 image tensors");
    }
    FILE* file = image_open_file(path, "wb");''',
    '''    if (image.dtype != dtype_uint8 && image.dtype != dtype_uint16) {
        throw std::invalid_argument("PNG can represent only uint8 or uint16 image tensors");
    }
    if (image.width > std::numeric_limits<png_uint_32>::max() ||
        image.height > std::numeric_limits<png_uint_32>::max()) {
        throw std::overflow_error("PNG dimensions exceed codec limits");
    }
    FILE* file = image_open_file(path, "wb");''',
)

once(
    '''    raw = static_cast<unsigned char*>(std::malloc(count == 0 ? 1 : count));
    if (!raw) throw std::bad_alloc();''',
    '''    raw = static_cast<unsigned char*>(std::malloc(count == 0 ? 1 : count));
    if (!raw) {
        jpeg_destroy_decompress(&info);
        created = false;
        std::fclose(file);
        throw std::bad_alloc();
    }''',
)

once(
    '''    if (quality < 1 || quality > 100) {
        throw std::invalid_argument("JPEG quality must be between 1 and 100");
    }
    auto raw = chw_to_hwc(image);''',
    '''    if (quality < 1 || quality > 100) {
        throw std::invalid_argument("JPEG quality must be between 1 and 100");
    }
    if (image.width > std::numeric_limits<JDIMENSION>::max() ||
        image.height > std::numeric_limits<JDIMENSION>::max()) {
        throw std::overflow_error("JPEG dimensions exceed codec limits");
    }
    auto raw = chw_to_hwc(image);''',
)

once(
    '''    const auto row_raw = checked_product(width, channels, "BMP row size overflow");
    const auto row = ((row_raw + 3) / 4) * 4;''',
    '''    const auto row_raw = checked_product(width, channels, "BMP row size overflow");
    if (row_raw > std::numeric_limits<std::size_t>::max() - 3) {
        throw std::overflow_error("BMP row size overflow");
    }
    const auto row = ((row_raw + 3) / 4) * 4;''',
)

once(
    '''    if (image.channels != 3 && image.channels != 4) {
        throw std::invalid_argument("BMP writer requires RGB or RGBA");
    }
    const auto row_raw = checked_product(image.width, image.channels, "BMP row size overflow");
    const auto row = ((row_raw + 3) / 4) * 4;''',
    '''    if (image.channels != 3 && image.channels != 4) {
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
    const auto row = ((row_raw + 3) / 4) * 4;''',
)

once(
    '''    if (quality < 1 || quality > 100) {
        throw std::invalid_argument("WebP quality must be between 1 and 100");
    }
    auto raw = chw_to_hwc(image);''',
    '''    if (quality < 1 || quality > 100) {
        throw std::invalid_argument("WebP quality must be between 1 and 100");
    }
    if (image.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        image.height > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        image.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) /
                          image.channels) {
        throw std::overflow_error("WebP dimensions exceed codec limits");
    }
    auto raw = chw_to_hwc(image);''',
)

path.write_text(text)
