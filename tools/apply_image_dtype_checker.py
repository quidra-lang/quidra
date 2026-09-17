from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one replacement target, found {count}")
    p.write_text(text.replace(old, new, 1))


checker = Path("src/checker.cpp")
text = checker.read_text()

old_try = """    } else if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
        auto source = check_expr(*node->value);
        if (poisoned(source)) {
"""
new_try = """    } else if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
        const auto error_type = simple(TypeKind::Error);
        Type source;
        if (expected) {
            const auto operand_expected = Type::union_of({*expected, error_type});
            source = check_expr(*node->value, &operand_expected);
        } else {
            source = check_expr(*node->value);
        }
        if (poisoned(source)) {
"""
if text.count(old_try) != 1:
    raise SystemExit(f"TryExpr target count = {text.count(old_try)}")
text = text.replace(old_try, new_try, 1)
text = text.replace(
    """        } else {
            auto error_type = simple(TypeKind::Error);
            if (source.kind != TypeKind::Union || case_index(source, error_type) < 0) {
""",
    """        } else {
            if (source.kind != TypeKind::Union || case_index(source, error_type) < 0) {
""",
    1,
)

read_start = text.index("                case BuiltinCallable::ImageRead: {")
write_start = text.index("                case BuiltinCallable::ImageWrite: {", read_start)
tensor_start = text.index("                case BuiltinCallable::TensorCreate:", write_start)

new_read = r'''                case BuiltinCallable::ImageRead: {
                    if (!node->type_arguments.empty()) {
                        error("GENERIC_TARGET",
                              "image.read does not take type arguments.", expression.span);
                    }
                    if (node->args.size() != 1) {
                        error("ARGUMENT_MISMATCH",
                              "image.read requires one path string.", expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }
                    auto string_type = simple(TypeKind::String);
                    auto path_type = check_expr(*node->args[0].value, &string_type);
                    if (node->args[0].writable ||
                        (node->args[0].name && *node->args[0].name != "path")) {
                        error("ARGUMENT_MISMATCH",
                              "image.read path has an invalid label or write capability.",
                              node->args[0].span);
                    }
                    if (poisoned(path_type)) {
                        type = simple(TypeKind::Invalid);
                        break;
                    }

                    const auto error_type = simple(TypeKind::Error);
                    const auto full_result = Type::union_of({
                        Type::tensor(simple(TypeKind::Int8)),
                        Type::tensor(simple(TypeKind::Int16)),
                        Type::tensor(simple(TypeKind::Int32)),
                        Type::tensor(simple(TypeKind::Int)),
                        Type::tensor(simple(TypeKind::UInt8)),
                        Type::tensor(simple(TypeKind::UInt16)),
                        Type::tensor(simple(TypeKind::UInt32)),
                        Type::tensor(simple(TypeKind::UInt64)),
                        Type::tensor(simple(TypeKind::Float32)),
                        Type::tensor(simple(TypeKind::Float)),
                        error_type});
                    type = full_result;

                    // An expected tensor<T> | error context selects a dtype without
                    // converting it. Runtime image decoding validates the exact dtype;
                    // a mismatch is the error alternative.
                    if (expected && expected->kind == TypeKind::Union &&
                        case_index(*expected, error_type) >= 0) {
                        std::vector<Type> non_error;
                        for (const auto& item : expected->cases) {
                            if (item != error_type) non_error.push_back(item);
                        }
                        if (non_error.size() == 1 &&
                            non_error.front().kind == TypeKind::Tensor &&
                            non_error.front().first && is_numeric(*non_error.front().first)) {
                            type = *expected;
                        }
                    }
                    break;
                }
'''

new_write = r'''                case BuiltinCallable::ImageWrite: {
                    if (!node->type_arguments.empty()) {
                        error("GENERIC_TARGET",
                              "image.write does not take type arguments.", expression.span);
                    }
                    if (node->args.size() < 2 || node->args.size() > 3) {
                        error("ARGUMENT_MISMATCH",
                              "image.write requires path, image, and optional quality = int.",
                              expression.span);
                        type = simple(TypeKind::Invalid);
                        break;
                    }
                    auto string_type = simple(TypeKind::String);
                    auto int_type = simple(TypeKind::Int);
                    auto path_type = check_expr(*node->args[0].value, &string_type);
                    auto value_type = check_expr(*node->args[1].value);
                    bool bad = poisoned(path_type) || poisoned(value_type);
                    if (!poisoned(value_type) &&
                        (value_type.kind != TypeKind::Tensor || !value_type.first ||
                         !is_numeric(*value_type.first))) {
                        error("TYPE_MISMATCH",
                              "image.write requires a numeric CHW tensor.",
                              node->args[1].span);
                    }
                    if (node->args[0].writable || node->args[1].writable ||
                        (node->args[0].name && *node->args[0].name != "path") ||
                        (node->args[1].name && *node->args[1].name != "image")) {
                        error("ARGUMENT_MISMATCH",
                              "image.write path/image arguments have invalid labels or write capability.",
                              expression.span);
                    }
                    if (node->args.size() == 3) {
                        if (node->args[2].writable || !node->args[2].name ||
                            *node->args[2].name != "quality") {
                            error("ARGUMENT_MISMATCH",
                                  "image.write third argument must be quality = value.",
                                  node->args[2].span);
                        }
                        bad |= poisoned(check_expr(*node->args[2].value, &int_type));
                    }
                    type = bad ? simple(TypeKind::Invalid)
                               : Type::union_of({simple(TypeKind::Void),
                                                 simple(TypeKind::Error)});
                    break;
                }
'''

text = text[:read_start] + new_read + new_write + text[tensor_start:]
checker.write_text(text)

# Existing codec round-trip tests know they are producing uint8 fixtures. Give
# those reads an explicit expected union so widening image.read's default union
# does not make the old matches non-exhaustive.
stdlib = Path("tests/stdlib_tests.sh")
stext = stdlib.read_text()
for name in ("png_read", "bmp_read", "tiff_read", "jpeg_read", "webp_read"):
    old = f'auto {name} = image.read('
    new = f'tensor<uint8> | error {name} = image.read('
    if stext.count(old) != 1:
        raise SystemExit(f"stdlib target {name}: {stext.count(old)}")
    stext = stext.replace(old, new, 1)
stdlib.write_text(stext)

print("applied image dtype checker patch")
