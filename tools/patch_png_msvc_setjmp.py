from pathlib import Path

path = Path("src/runtime_image.cpp")
text = path.read_text()
old = '''    std::uint8_t* raw = nullptr;
    png_bytep* rows = nullptr;

    if (setjmp(error.jump) != 0) {
        std::free(rows);
        std::free(raw);
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(file);
        throw std::runtime_error(
            std::string("PNG decode failed: ") +
            (error.message[0] ? error.message : path));
    }

    png_init_io(png, file);'''
new = '''    std::uint8_t* raw = nullptr;
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

    png_init_io(png, file);'''
if text.count(old) != 1:
    raise SystemExit(f"expected one PNG setjmp block, got {text.count(old)}")
path.write_text(text.replace(old, new, 1))
