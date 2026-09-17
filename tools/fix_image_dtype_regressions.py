from pathlib import Path

# Fix Quidra test source comments and expected output construction.
tests = Path("tests/image_dtype_tests.sh")
text = tests.read_text()
text = text.replace('# uint16 PNG writing must preserve the exact 16-bit sample.\n',
                    '// uint16 PNG writing must preserve the exact 16-bit sample.\n')
text = text.replace('# A target format must reject a dtype it cannot represent instead of narrowing.\n',
                    '// A target format must reject a dtype it cannot represent instead of narrowing.\n')
text = text.replace(
    'tiff_expected="$(printf \'%s\\n\' -8 -1600 -320000 -640000 8 1600 320000 640000 1.5 2.5 4660 rejected | sed \'$d\')"',
    'tiff_expected="$(printf \'%s\\n\' -8 -1600 -320000 -640000 8 1600 320000 640000 1.5 2.5 4660 rejected)"')
tests.write_text(text)

# Avoid -Wunused-variable under warnings-as-errors configurations.
backend = Path("src/llvm_backend.cpp")
btext = backend.read_text()
old = '''            for(const auto& item:image_cases)\n                case_labels.push_back(unique_label("image.read.dtype"));'''
new = '''            for(std::size_t i=0;i<image_cases.size();++i)\n                case_labels.push_back(unique_label("image.read.dtype"));'''
if old not in btext:
    raise SystemExit("LLVM image dtype label loop target missing")
btext = btext.replace(old, new, 1)
backend.write_text(btext)

print("fixed image dtype regression tests and warning")
