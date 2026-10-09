#include "quidra/abi/error_record.hpp"
#include <cstdio>
#include <cstdlib>

// Keep schema checks and their observable calls active under NDEBUG.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "error schema check failed at %s:%d: %s\n", \
                     __FILE__, __LINE__, #condition); \
        std::abort(); \
    } \
} while (false)
#include <cstddef>
#include <string_view>

using namespace quidra::abi;

int main() {
    CHECK(error_value_schema(0) == nullptr);
    CHECK(error_value_schema(22) == nullptr);
    for (std::uint32_t i = 1; i <= error_value_codes.size(); ++i) {
        const auto* schema = error_value_schema(i);
        CHECK(schema != nullptr);
        CHECK(static_cast<std::uint32_t>(schema->code) == i);
        CHECK(is_core_error_code(schema->spelling));
        for (std::size_t j = 0; j < schema->details.size(); ++j) {
            const auto& detail = schema->details[j];
            CHECK(!detail.name.empty());
            CHECK(detail.identity);
            if (detail.kind == ErrorDetailKind::integer) CHECK(!detail.optional);
            for (std::size_t k = j + 1; k < schema->details.size(); ++k)
                CHECK(detail.name != schema->details[k].name);
        }
    }
    const auto* file = error_value_schema(static_cast<std::uint32_t>(ErrorValueCode::file_read));
    CHECK(file->details.size() == 2);
    CHECK(file->details[0].name == "path" && file->details[0].optional);
    CHECK(file->details[1].name == "reason" && !file->details[1].optional);
    const auto* json = error_value_schema(static_cast<std::uint32_t>(ErrorValueCode::json_parse));
    CHECK(json->details[1].kind == ErrorDetailKind::integer);
    CHECK(valid_user_error_code("A"));
    CHECK(valid_user_error_code("A9_B2"));
    CHECK(!valid_user_error_code("9A"));
    CHECK(!valid_user_error_code("A__B"));
    CHECK(!valid_user_error_code("A_"));
    CHECK(!valid_user_error_code("a_B"));
    CHECK(is_core_error_code("INDEX_BOUNDS"));
    CHECK(!valid_custom_error_code("INDEX_BOUNDS"));
    CHECK(!valid_custom_error_code("NN", "NN"));
    CHECK(valid_custom_error_code("NN_SHAPE", "NN"));
    CHECK(!valid_custom_error_code("MATH_SHAPE", "NN"));

    const std::array<ErrorDetailValue, 2> valid_read{
        ErrorDetailValue::text_value("data.csv"),
        ErrorDetailValue::name_value("NOT_FOUND")};
    std::string message = "untouched";
    CHECK(render_error(*file, valid_read, message));
    CHECK(message == "cannot read \"data.csv\": not found");
    const std::array<ErrorDetailValue, 2> invalid_read{
        ErrorDetailValue::text_value("data.csv"),
        ErrorDetailValue::text_value("NOT_FOUND")};
    CHECK(!render_error(*file, invalid_read, message));
    CHECK(message == "cannot read \"data.csv\": not found");
    const std::array<ErrorDetailValue, 2> no_path{
        ErrorDetailValue::absent(ErrorDetailKind::text),
        ErrorDetailValue::name_value("HANDLE_CLOSED")};
    CHECK(render_error(*file, no_path, message));
    CHECK(message == "cannot read the file handle: handle closed");
    const std::array<ErrorDetailValue, 2> parse_values{
        ErrorDetailValue::name_value("real32"),
        ErrorDetailValue::name_value("OUT_OF_RANGE")};
    CHECK(render_error(*error_value_schema(
        static_cast<std::uint32_t>(ErrorValueCode::numeric_parse)), parse_values, message));
    CHECK(message == "cannot parse numeric input as real32: value out of range");
    const std::array<ErrorDetailValue, 2> json_values{
        ErrorDetailValue::text_value("unexpected token"), ErrorDetailValue::integer_value(17)};
    CHECK(render_error(*json, json_values, message));
    CHECK(message == "unexpected token at byte 17");
    const std::array<ErrorDetailValue, 1> left_text{ErrorDetailValue::text_value("failure")};
    const std::array<ErrorDetailValue, 1> other_text{ErrorDetailValue::text_value("different")};
    const ErrorValueIdentity cause{"CAUSE", left_text, nullptr};
    const ErrorValueIdentity first{"DATASET_LOAD", left_text, &cause};
    const ErrorValueIdentity equal{"DATASET_LOAD", left_text, &cause};
    const ErrorValueIdentity unequal{"DATASET_LOAD", other_text, &cause};
    const ErrorValueIdentity no_cause{"DATASET_LOAD", left_text, nullptr};
    CHECK(same_error_identity(first, equal));
    CHECK(!same_error_identity(first, unequal));
    CHECK(!same_error_identity(first, no_cause));
}
