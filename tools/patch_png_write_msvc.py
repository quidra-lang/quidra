from pathlib import Path

path = Path("src/runtime_image.cpp")
text = path.read_text()
old = '''    png_bytep* rows = nullptr;\n    if (setjmp(error.jump) != 0) {\n        std::free(rows);\n        png_destroy_write_struct(&png, &info);\n        std::fclose(file);\n        throw std::runtime_error(\n            std::string("PNG encode failed: ") +\n            (error.message[0] ? error.message : path));\n    }\n'''
new = '''    png_bytep* rows = nullptr;\n#ifdef _MSC_VER\n#pragma warning(push)\n#pragma warning(disable: 4611)\n#endif\n    if (setjmp(error.jump) != 0) {\n        std::free(rows);\n        png_destroy_write_struct(&png, &info);\n        std::fclose(file);\n        throw std::runtime_error(\n            std::string("PNG encode failed: ") +\n            (error.message[0] ? error.message : path));\n    }\n#ifdef _MSC_VER\n#pragma warning(pop)\n#endif\n'''
if text.count(old) != 1:
    raise SystemExit(f"expected exactly one write_png setjmp block, found {text.count(old)}")
path.write_text(text.replace(old, new))
