#include "profile_cli.hpp"
#include "platform/private_directory.hpp"
#include "platform/strict_json.hpp"
#include "platform/user_cache_directory.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <variant>

namespace quidra::cli {
namespace {
namespace fs = std::filesystem;
using U64 = std::uint64_t;
[[noreturn]] void invalid(const std::string& message) {
    throw std::runtime_error("Profile: " + message);
}
struct Number { std::string text; };
struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    std::variant<std::nullptr_t, bool, Number, std::string, Array, Object> value;
};

// The shared reader's schemas use signed integers only. Retain number lexemes
// here because runtime counters use uint64 and Chrome uses fractional display
// timestamps; share its Unicode string decoder rather than duplicating it.
class Reader {
    std::string_view text;
    std::size_t at{};
    void space() {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\n' ||
               text[at] == '\r' || text[at] == '\t')) ++at;
    }
    bool take(char c) {
        space(); if (at == text.size() || text[at] != c) return false;
        ++at; return true;
    }
    void require(char c) { if (!take(c)) invalid("malformed JSON"); }
    std::string string() {
        space(); const auto begin = at; require('"');
        while (at < text.size()) {
            const char c = text[at++];
            if (c == '"') {
                auto decoded = platform::read_strict_json(text.substr(begin, at - begin));
                return std::get<std::string>(std::move(decoded.data));
            }
            if (c == '\\') {
                if (at == text.size()) invalid("unterminated JSON string");
                ++at;
            }
        }
        invalid("unterminated JSON string");
    }
    Json number() {
        const auto begin = at;
        if (text[at] == '-') ++at;
        if (at == text.size()) invalid("malformed JSON number");
        if (text[at] == '0') ++at;
        else {
            if (text[at] < '1' || text[at] > '9') invalid("malformed JSON number");
            while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
        }
        if (at < text.size() && text[at] == '.') {
            ++at; const auto fraction = at;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
            if (at == fraction) invalid("malformed JSON fraction");
        }
        if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
            ++at;
            if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
            const auto exponent = at;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
            if (at == exponent) invalid("malformed JSON exponent");
        }
        return {Number{std::string(text.substr(begin, at - begin))}};
    }
    Json value(std::size_t depth) {
        if (depth > 64) invalid("JSON nesting exceeds 64");
        space(); if (at == text.size()) invalid("unexpected end of JSON");
        if (text[at] == '"') return {string()};
        if (take('{')) {
            Json::Object result;
            if (!take('}')) {
                do {
                    if (at == text.size()) invalid("unexpected end of JSON object");
                    auto key = string(); require(':');
                    if (!result.emplace(std::move(key), value(depth + 1)).second)
                        invalid("duplicate JSON key");
                } while (take(','));
                require('}');
            }
            return {std::move(result)};
        }
        if (take('[')) {
            Json::Array result;
            if (!take(']')) {
                do { result.push_back(value(depth + 1)); } while (take(','));
                require(']');
            }
            return {std::move(result)};
        }
        for (const auto& [word, boolean] : {std::pair{"true", true}, {"false", false}}) {
            if (text.substr(at, std::char_traits<char>::length(word)) == word) {
                at += std::char_traits<char>::length(word); return {boolean};
            }
        }
        if (text.substr(at, 4) == "null") { at += 4; return {nullptr}; }
        if (text[at] == '-' || (text[at] >= '0' && text[at] <= '9')) return number();
        invalid("malformed JSON value");
    }
public:
    explicit Reader(std::string_view input) : text(input) {}
    Json read() {
        auto result = value(0); space();
        if (at != text.size()) invalid("trailing JSON data");
        return result;
    }
};
template<class T> const T& as(const Json& value, const char* name) {
    const auto result = std::get_if<T>(&value.value);
    if (!result) invalid(std::string("invalid ") + name + " type");
    return *result;
}
const Json& field(const Json& value, const char* name) {
    const auto& object = as<Json::Object>(value, "object");
    const auto found = object.find(name);
    if (found == object.end()) invalid(std::string("missing ") + name);
    return found->second;
}
const Json* optional_field(const Json& value, const char* name) {
    const auto& object = as<Json::Object>(value, "object");
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}
U64 unsigned_number(std::string_view number, const char* name) {
    if (number.empty() || std::any_of(number.begin(), number.end(),
            [](char c) { return c < '0' || c > '9'; }))
        invalid(std::string(name) + " must be an unsigned integer");
    U64 result{};
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
        invalid(std::string(name) + " exceeds uint64");
    return result;
}
U64 integer(const Json& value, const char* name) {
    return unsigned_number(as<Number>(value, name).text, name);
}
U64 count(const Json& value, const char* name) { return integer(field(value, name), name); }
bool flag(const Json& value, const char* name) { return as<bool>(field(value, name), name); }
const std::string& text(const Json& value, const char* name) {
    return as<std::string>(field(value, name), name);
}
const Json::Array& array(const Json& value, const char* name) {
    return as<Json::Array>(field(value, name), name);
}
bool add(U64& total, U64 value) {
    if (value > std::numeric_limits<U64>::max() - total) return false;
    total += value; return true;
}
void version(const Json& value) {
    if (count(value, "schema") != 1 || text(value, "clock_units") != "ns")
        invalid("unsupported report schema or clock units");
}
void drops(const Json& value, bool& complete, U64& invalid_drops, U64& storage_drops) {
    complete = flag(value, "complete");
    invalid_drops = count(field(value, "dropped_records"), "invalid");
    storage_drops = count(field(value, "dropped_records"), "storage");
    if (complete && (invalid_drops || storage_drops)) invalid("inconsistent complete/drop metadata");
}
void display_number(const Json& value, const char* name) {
    const auto& number = as<Number>(field(value, name), name).text;
    if (number.front() == '-') invalid(std::string("negative ") + name);
}
void source(const Json& value, ProfileRow& row) {
    const auto& site = field(value, "site");
    row.file = text(site, "file"); row.line = count(site, "line"); row.column = count(site, "column");
    if (text(site, "node_id") != row.node) invalid("aggregate/source node mismatch");
}
auto key(const ProfileRow& row) { return std::tie(row.node, row.owner, row.kernel); }

struct Boundary { U64 step{}, host{}; bool available{}; };
struct Wait { U64 step{}, begin{}, end{}; bool available{}; };
struct Gpu { U64 device{}, serial{}, step{}, begin{}, end{}; bool available{}, sampled{}; };
void ordered(U64 begin, U64 end, bool available) {
    if (end < begin || (available && begin == 0) || (!available && (begin != 0 || end != 0)))
        invalid("inconsistent timestamp availability/order");
}
std::vector<Gpu> gpu_intervals(const Json& root, const char* name, bool sampled) {
    std::vector<Gpu> result;
    std::set<std::tuple<U64, U64, U64>> unique;
    for (const auto& value : array(root, name)) {
        Gpu interval{count(value, "device"), count(value, "serial"),
            count(value, sampled ? "step" : "commit_step"), count(value, "gpu_begin_ns"),
            count(value, "gpu_end_ns"), flag(value, "timing_available"), sampled};
        if (interval.device > static_cast<U64>(std::numeric_limits<std::int32_t>::max()) ||
            interval.step > std::numeric_limits<std::uint32_t>::max()) invalid("device/step out of range");
        ordered(interval.begin, interval.end, interval.available);
        // A buffer is committed once. Its blit/compute encoder samples may
        // legitimately share device, serial and step while naming distinct
        // time intervals, so that tuple is not a sample identity.
        if (!sampled && !unique.emplace(interval.device, interval.serial, 0).second)
            invalid("duplicate GPU interval identity");
        result.push_back(interval);
    }
    return result;
}
std::string micros(U64 value, bool half = false) {
    std::ostringstream output;
    output << value / 1000 << '.' << std::setw(4) << std::setfill('0')
           << (value % 1000) * 10 + (half ? 5 : 0) << "us";
    return output.str();
}
std::string median(std::vector<U64> values) {
    if (values.empty()) return "unavailable";
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    if (values.size() % 2) return micros(values[middle]);
    const auto a = values[middle - 1], b = values[middle];
    return micros(a / 2 + b / 2 + (a % 2 + b % 2) / 2, (a % 2 + b % 2) % 2 != 0);
}

struct Paths { fs::path trace, timeline; };
fs::path destination(const fs::path& value) {
    if (value.empty() || value == "-" || value == "stderr") invalid("reports require file paths");
    const auto absolute = fs::absolute(value);
    const auto status = fs::symlink_status(absolute);
    if (fs::exists(status) && !fs::is_regular_file(status)) invalid("report destination is not a regular file");
    fs::create_directories(absolute.parent_path());
    return fs::weakly_canonical(absolute);
}
fs::path report_directory() {
    const auto cache = platform::user_cache_directory();
    if (!cache) invalid("no user cache directory; supply --trace and --timeline");
    auto root = platform::open_private_root(*cache);
    if (!root.path) invalid(root.refusal);
    auto profiles = platform::open_private_subdirectory(*root.path, "profiles");
    if (!profiles.path) invalid(profiles.refusal);
    static std::atomic<U64> sequence{};
    std::random_device random;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(random()) + "-" + std::to_string(sequence.fetch_add(1));
        if (fs::exists(*profiles.path / nonce)) continue;
        auto session = platform::open_private_subdirectory(*profiles.path, nonce);
        if (session.path) return *session.path;
        invalid(session.refusal);
    }
    invalid("cannot allocate a unique report directory");
}
Paths report_paths(const ProfileOptions& options, const fs::path& source) {
    std::optional<fs::path> session;
    if (!options.trace || !options.timeline) session = report_directory();
    Paths paths{destination(options.trace ? *options.trace : *session / "trace.json"),
                destination(options.timeline ? *options.timeline : *session / "timeline.json")};
    if (paths.trace == paths.timeline || (fs::exists(paths.trace) && fs::exists(paths.timeline) &&
            fs::equivalent(paths.trace, paths.timeline))) invalid("trace and timeline must be distinct files");
    const auto source_path = fs::weakly_canonical(fs::absolute(source));
    for (const auto& report : {paths.trace, paths.timeline})
        if (report == source_path || (fs::exists(report) && fs::exists(source_path) &&
                fs::equivalent(report, source_path))) invalid("report destination aliases program source");
    // An early child failure must never summarize a previous run's reports.
    fs::remove(paths.trace); fs::remove(paths.timeline);
    return paths;
}
std::string read_file(const fs::path& path) {
    constexpr std::uintmax_t limit = 64 * 1024 * 1024;
    if (!fs::is_regular_file(fs::symlink_status(path)) || fs::file_size(path) > limit)
        invalid("report missing, not regular, or larger than 64 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input) invalid("report unreadable");
    std::string result(static_cast<std::size_t>(fs::file_size(path)), '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) invalid("report changed while reading");
    return result;
}
void top(const ProfileSummary& summary, U64 limit, bool gpu, std::ostream& output) {
    if (gpu && !summary.typed_gpu) {
        output << "Top GPU activities: unavailable; legacy schema 1 aggregates contain unclassified GPU spans\n";
        return;
    }
    std::vector<const ProfileRow*> rows;
    for (const auto& row : summary.rows)
        if (gpu ? row.gpu_calls != 0 : row.host_calls != 0) rows.push_back(&row);
    std::sort(rows.begin(), rows.end(), [gpu](const auto* left, const auto* right) {
        const auto a = gpu ? left->gpu_ns : left->host_ns;
        const auto b = gpu ? right->gpu_ns : right->host_ns;
        return a != b ? a > b : key(*left) < key(*right);
    });
    output << (gpu ? "Top GPU activities" : "Top host") << " (known timings):\n";
    if (rows.empty()) output << "  unavailable\n";
    U64 emitted{};
    for (const auto* row : rows) {
        if (emitted++ == limit) break;
        output << "  " << row->kernel << " node=" << row->node << " owner=" << row->owner
            << " time=" << micros(gpu ? row->gpu_ns : row->host_ns)
            << " calls=" << (gpu ? row->activity_calls : row->calls)
            << " timed=" << (gpu ? row->gpu_calls : row->host_calls)
            << " unknown=" << (gpu ? row->gpu_unknown_calls : row->host_unknown_calls)
            << " bytes=" << (gpu ? row->activity_bytes : row->bytes);
        if (gpu ? row->activity_overflow : row->overflow) output << " saturated";
        if (!row->file.empty()) output << " " << row->file << ':' << row->line << ':' << row->column;
        output << '\n';
    }
    U64 unknown{}; bool overflow = false;
    for (const auto& row : summary.rows)
        if (!add(unknown, gpu ? row.gpu_unknown_calls : row.host_unknown_calls)) overflow = true;
    output << "  unknown timing calls=" << (overflow ? ">=" : "") << unknown << '\n';
}
} // namespace

ProfileOptions parse_profile_options(const std::vector<std::string>& arguments) {
    ProfileOptions options;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        if (argument == "--") {
            options.program_arguments.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i + 1), arguments.end());
            break;
        }
        if (!seen.insert(argument).second) invalid("duplicate option " + argument);
        if (argument == "--split") options.split = true;
        else if (argument == "--roofline") options.roofline = true;
        else if (argument == "--top" || argument == "--trace" || argument == "--timeline") {
            if (++i == arguments.size()) invalid("missing value for " + argument);
            if (argument == "--top") {
                options.top = unsigned_number(arguments[i], "--top");
                if (!options.top) invalid("--top must be positive");
            } else if (argument == "--trace") options.trace = arguments[i];
            else options.timeline = arguments[i];
        } else invalid("unknown option " + argument);
    }
    return options;
}

ProfileSummary read_profile_report(std::string_view document) {
    const auto root = Reader(document).read();
    const auto schema = count(root, "schema");
    if ((schema != 1 && schema != 2) || text(root, "clock_units") != "ns")
        invalid("unsupported profile schema or clock units");
    ProfileSummary result;
    result.typed_gpu = schema == 2;
    if (result.typed_gpu && (text(root, "gpu_aggregation") != "typed_activity_container" ||
            text(root, "bytes_scope") != "inclusive_events"))
        invalid("unsupported profile aggregate semantics");
    drops(root, result.complete, result.invalid_drops, result.storage_drops);
    result.open_ranges = count(root, "open_ranges");
    if (result.complete && result.open_ranges) invalid("complete profile has open ranges");
    const auto& configuration = field(root, "configuration");
    result.split_requested = flag(configuration, "split_requested");
    const auto& capture = field(configuration, "capture_step");
    if (!std::holds_alternative<std::nullptr_t>(capture.value))
        result.capture_step = integer(capture, "capture_step");
    result.device_instrumentation = text(configuration, "device_instrumentation");
    if (const auto* capture_status = optional_field(root, "capture"); capture_status &&
            !std::holds_alternative<std::nullptr_t>(capture_status->value)) {
        ProfileSummary::Capture status;
        status.step = count(*capture_status, "requested_step");
        if (!result.capture_step || status.step != *result.capture_step)
            invalid("capture step disagrees with configuration");
        status.state = text(*capture_status, "state");
        if (status.state != "off" && status.state != "armed" &&
                status.state != "not_reached" && status.state != "no_metal_work" &&
                status.state != "unsupported" && status.state != "active" &&
                status.state != "stopped" && status.state != "partial" &&
                status.state != "unavailable" && status.state != "interrupted")
            invalid("invalid capture state");
        status.output_path = text(*capture_status, "output_path");
        status.snapshot_complete = flag(*capture_status, "snapshot_complete");
        const auto nullable_flag = [&](const char* name) -> std::optional<bool> {
            const auto& value = field(*capture_status, name);
            if (std::holds_alternative<std::nullptr_t>(value.value)) return std::nullopt;
            return as<bool>(value, name);
        };
        status.start_succeeded = nullable_flag("start_succeeded");
        status.stopped = nullable_flag("stopped");
        status.exact_core_step = nullable_flag("exact_core_step");
        status.external_queues_verified = nullable_flag("external_queues_verified");
        status.artifact_exists = nullable_flag("artifact_exists");
        const auto& commits = field(*capture_status, "extra_commits");
        if (!std::holds_alternative<std::nullptr_t>(commits.value))
            status.extra_commits = integer(commits, "extra_commits");
        // An incomplete snapshot may retain an older state. Validate claims
        // only when the producer certifies that the mutable fields are current.
        if (status.snapshot_complete) {
            if ((status.exact_core_step == true &&
                    (status.start_succeeded != true || status.stopped != true || status.state != "stopped")) ||
                    (status.stopped == true && status.start_succeeded != true) ||
                    (status.state == "stopped" &&
                        (status.start_succeeded != true || status.stopped != true)))
                invalid("inconsistent capture completion status");
        }
        result.capture = std::move(status);
    }
    for (const auto& event : array(root, "traceEvents")) {
        const auto& phase = text(event, "ph");
        (void)text(event, "name"); (void)count(event, "pid"); (void)count(event, "tid");
        if (phase == "X") {
            display_number(event, "ts"); display_number(event, "dur");
            const auto& args = field(event, "args");
            (void)as<Json::Object>(args, "trace args");
            if (result.typed_gpu) {
                const auto& role = text(args, "gpu_role");
                if (role != "none" && role != "activity" && role != "container") invalid("invalid trace GPU role");
            }
        } else if (phase == "M") (void)as<Json::Object>(field(event, "args"), "metadata args");
        else invalid("unsupported Chrome event phase");
    }
    std::set<std::tuple<std::string, std::string, std::string>> unique;
    for (const auto& value : array(root, "aggregate")) {
        ProfileRow row;
        row.node = text(value, "node_id"); row.owner = text(value, "owner"); row.kernel = text(value, "kernel");
        row.calls = count(value, "calls"); row.host_calls = count(value, "host_calls"); row.gpu_calls = count(value, "gpu_calls");
        row.host_unknown_calls = count(value, "host_unknown_calls"); row.gpu_unknown_calls = count(value, "gpu_unknown_calls");
        row.host_ns = count(value, "host_ns"); row.gpu_ns = count(value, "gpu_ns"); row.bytes = count(value, "bytes");
        row.overflow = flag(value, "overflow"); source(value, row);
        display_number(value, "host_us"); display_number(value, "gpu_us");
        if (result.typed_gpu) {
            row.activity_calls = count(value, "activity_calls");
            row.activity_gpu_known_calls = count(value, "activity_gpu_known_calls");
            row.activity_gpu_unknown_calls = count(value, "activity_gpu_unknown_calls");
            row.activity_gpu_ns = count(value, "activity_gpu_ns"); row.activity_bytes = count(value, "activity_bytes");
            row.activity_overflow = flag(value, "activity_overflow");
            row.container_calls = count(value, "container_calls");
            row.container_gpu_known_calls = count(value, "container_gpu_known_calls");
            row.container_gpu_unknown_calls = count(value, "container_gpu_unknown_calls");
            row.container_gpu_ns = count(value, "container_gpu_ns"); row.container_bytes = count(value, "container_bytes");
            row.container_overflow = flag(value, "container_overflow");
            const auto consistent = [](U64 calls, U64 known, U64 unknown, U64 ns, U64 bytes, bool overflow) {
                return known <= calls && unknown == calls - known && (known || !ns) &&
                    (calls || (!bytes && !overflow)) && (!overflow ||
                        ns == std::numeric_limits<U64>::max() || bytes == std::numeric_limits<U64>::max());
            };
            if (!consistent(row.activity_calls, row.activity_gpu_known_calls, row.activity_gpu_unknown_calls,
                    row.activity_gpu_ns, row.activity_bytes, row.activity_overflow) ||
                !consistent(row.container_calls, row.container_gpu_known_calls, row.container_gpu_unknown_calls,
                    row.container_gpu_ns, row.container_bytes, row.container_overflow) ||
                row.activity_calls > row.calls || row.container_calls > row.calls - row.activity_calls ||
                row.gpu_calls != row.activity_gpu_known_calls || row.gpu_unknown_calls != row.activity_gpu_unknown_calls ||
                row.gpu_ns != row.activity_gpu_ns || row.activity_bytes > row.bytes || row.container_bytes > row.bytes ||
                ((row.activity_overflow || row.container_overflow) && !row.overflow))
                invalid("inconsistent typed GPU metrics");
        }
        if (!row.calls || row.host_calls > row.calls || row.gpu_calls > row.calls ||
                row.host_unknown_calls != row.calls - row.host_calls ||
                (!result.typed_gpu && row.gpu_unknown_calls != row.calls - row.gpu_calls) ||
                (!row.host_calls && row.host_ns) || (!row.gpu_calls && row.gpu_ns))
            invalid("inconsistent aggregate timing counts");
        if (!unique.emplace(row.node, row.owner, row.kernel).second) invalid("duplicate aggregate key");
        result.rows.push_back(std::move(row));
    }
    return result;
}

TimelineSummary read_timeline_report(std::string_view document) {
    const auto root = Reader(document).read(); version(root);
    TimelineSummary result; drops(root, result.complete, result.invalid_drops, result.storage_drops);
    (void)text(root, "clock"); (void)text(root, "step_attribution");
    (void)array(root, "marks"); (void)array(root, "warmups"); (void)array(root, "compiles");
    if (const auto* complete = optional_field(root, "gpu_attribution_complete"))
        result.gpu_attribution_complete = as<bool>(*complete, "gpu_attribution_complete");
    if (const auto* complete = optional_field(root, "gpu_leaf_attribution_complete"))
        result.gpu_leaf_attribution_complete = as<bool>(*complete, "gpu_leaf_attribution_complete");
    if (const auto* gaps = optional_field(root, "gpu_sampling_gaps"))
        for (const auto& value : as<Json::Array>(*gaps, "gpu_sampling_gaps")) {
            TimelineSummary::SamplingGap gap;
            gap.device = count(value, "device"); gap.serial = count(value, "serial"); gap.step = count(value, "step");
            gap.reason = text(value, "reason"); gap.compute = flag(value, "compute");
            gap.additional_encoder = flag(value, "additional_encoder");
            if (const auto* incomplete = optional_field(value, "timing_incomplete"))
                gap.timing_incomplete = as<bool>(*incomplete, "timing_incomplete");
            if (gap.device > static_cast<U64>(std::numeric_limits<std::int32_t>::max()) ||
                    gap.step > std::numeric_limits<std::uint32_t>::max() || gap.reason.empty())
                invalid("invalid GPU sampling gap");
            result.sampling_gaps.push_back(std::move(gap));
        }
    if ((result.gpu_leaf_attribution_complete == true &&
            (result.gpu_attribution_complete != true || !result.sampling_gaps.empty())) ||
        (result.gpu_attribution_complete == true &&
            std::any_of(result.sampling_gaps.begin(), result.sampling_gaps.end(),
                [](const auto& gap) { return gap.timing_incomplete; })))
        invalid("inconsistent GPU attribution completeness");
    std::map<U64, Boundary> boundaries;
    for (const auto& value : array(root, "steps")) {
        if (text(value, "point") != "step_boundary") invalid("non-boundary in steps");
        Boundary boundary{count(value, "step"), count(value, "host_ns"), flag(value, "host_available")};
        if (boundary.step > std::numeric_limits<std::uint32_t>::max() ||
                boundary.available != (boundary.host != 0) || !boundaries.emplace(boundary.step, boundary).second)
            invalid("invalid step boundary");
    }
    std::vector<Wait> waits;
    for (const auto& value : array(root, "waits")) {
        Wait wait{count(value, "step"), count(value, "begin_ns"), count(value, "end_ns"), flag(value, "timing_available")};
        if (wait.step > std::numeric_limits<std::uint32_t>::max()) invalid("wait step out of range");
        ordered(wait.begin, wait.end, wait.available); waits.push_back(wait);
    }
    auto gpu = gpu_intervals(root, "gpu", false);
    std::map<std::pair<U64, U64>, U64> commits;
    for (const auto& buffer : gpu) commits.emplace(std::pair{buffer.device, buffer.serial}, buffer.step);
    auto samples = gpu_intervals(root, "gpu_steps", true);
    result.sampling_attempted = !samples.empty();
    result.sampled = std::any_of(samples.begin(), samples.end(), [](const auto& interval) { return interval.available; });
    result.unavailable_samples = std::any_of(samples.begin(), samples.end(), [](const auto& interval) { return !interval.available; });
    result.approximate = samples.empty() && !gpu.empty();
    if (result.sampling_attempted) {
        // A partial sampled stream cannot silently hide unsampled buffers.
        for (const auto& interval : gpu)
            if (std::none_of(samples.begin(), samples.end(), [&](const auto& sample) {
                    return sample.device == interval.device && sample.serial == interval.serial;
                })) { samples.push_back(interval); result.approximate = true; }
        gpu = std::move(samples);
    }
    struct IncompleteRange { U64 first{}, end{}; };
    std::vector<IncompleteRange> incomplete;
    for (const auto& gap : result.sampling_gaps) {
        if (!gap.timing_incomplete) continue;
        std::optional<U64> next;
        for (const auto& interval : gpu)
            if (interval.sampled && interval.available && interval.device == gap.device && interval.step > gap.step &&
                std::none_of(result.sampling_gaps.begin(), result.sampling_gaps.end(), [&](const auto& other) {
                    return other.timing_incomplete && other.device == interval.device &&
                        other.serial == interval.serial && other.step == interval.step;
                }) && (!next || interval.step < *next)) next = interval.step;
        const auto commit = commits.find({gap.device, gap.serial});
        const auto window_end = boundaries.empty() ? gap.step + 1 : boundaries.rbegin()->first + 1;
        // A held encoder can span several boundaries, while its single row
        // retains only the start step. Missing later rows are unknown work,
        // including when another participating device has valid rows there.
        const auto finish = next ? *next : commit != commits.end()
            ? std::max(gap.step, commit->second) + 1 : window_end;
        if (gap.step < window_end) incomplete.push_back({gap.step, std::min(finish, window_end)});
        result.unavailable_samples |= result.sampling_attempted;
    }
    const auto incomplete_step = [&](U64 step) {
        return std::any_of(incomplete.begin(), incomplete.end(), [step](const auto& range) {
            return range.first <= step && step < range.end;
        });
    };
    if (!result.complete) return result;
    std::map<U64, std::map<U64, std::vector<Gpu>>> by_step;
    for (const auto& interval : gpu) by_step[interval.step][interval.device].push_back(interval);
    for (auto current = boundaries.begin(); current != boundaries.end(); ++current) {
        if (current == boundaries.begin()) continue;
        const auto previous = std::prev(current);
        const auto& start = previous->second; const auto& end = current->second;
        if (end.step != start.step + 1) continue;
        if (start.available && end.available) {
            if (end.host < start.host) invalid("nonmonotonic step host timestamps");
            U64 waiting{}; bool known = true;
            for (const auto& wait : waits) {
                if (!wait.available) { if (wait.step == end.step) known = false; continue; }
                const auto begin = std::max(start.host, wait.begin), finish = std::min(end.host, wait.end);
                if (finish > begin && !add(waiting, finish - begin)) known = false;
            }
            if (known) {
                result.wait_ns.push_back(waiting);
                if (waiting <= end.host - start.host) result.host_ns.push_back(end.host - start.host - waiting);
            }
        }
        if (incomplete_step(end.step)) continue;
        const auto found = by_step.find(end.step);
        if (found == by_step.end() || found->second.empty()) continue;
        U64 busy{}, idle{}; bool busy_known = true, idle_known = !incomplete_step(start.step);
        for (auto& [device, intervals] : found->second) {
            std::sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& b) {
                return std::tie(a.begin, a.end, a.serial) < std::tie(b.begin, b.end, b.serial);
            });
            U64 last{};
            const auto before = by_step.find(start.step);
            if (before == by_step.end() || !before->second.contains(device)) idle_known = false;
            else {
                for (const auto& interval : before->second.at(device)) {
                    if (!interval.available) idle_known = false;
                    last = std::max(last, interval.end);
                }
            }
            for (const auto& interval : intervals) {
                if (!interval.available) { busy_known = false; idle_known = false; continue; }
                if (!add(busy, interval.end - interval.begin)) busy_known = false;
                if (interval.begin > last && !add(idle, interval.begin - last)) idle_known = false;
                last = std::max(last, interval.end);
            }
        }
        if (busy_known) result.gpu_ns.push_back(busy);
        if (idle_known) result.idle_ns.push_back(idle);
    }
    return result;
}

int run_profile_cli(const fs::path& source, const std::vector<std::string>& arguments,
        const CachedProfileRunner& runner, std::ostream& output, std::ostream& errors) {
    ProfileOptions options; Paths paths;
    try {
        options = parse_profile_options(arguments);
        if (!runner) invalid("no cached program runner");
        paths = report_paths(options, source);
    } catch (const std::exception& error) { errors << error.what() << '\n'; return 2; }
    const RuntimeEnvironment environment{{"QUIDRA_PROFILE", paths.trace.string()},
        {"QUIDRA_TIMELINE", paths.timeline.string()},
        {"QUIDRA_PROFILE_SPLIT", options.split ? std::optional<std::string>{"1"} : std::nullopt}};
    const int status = runner(source, options.program_arguments, environment);
    output << "Trace: " << paths.trace.string() << "\nTimeline: " << paths.timeline.string() << '\n';
    try {
        const auto summary = read_timeline_report(read_file(paths.timeline));
        output << "Step medians: H=" << median(summary.host_ns) << " W=" << median(summary.wait_ns)
            << " G=" << median(summary.gpu_ns) << " I=" << median(summary.idle_ns) << '\n';
        output << "Samples: H=" << summary.host_ns.size() << " W=" << summary.wait_ns.size()
            << " G=" << summary.gpu_ns.size() << " I=" << summary.idle_ns.size()
            << "; initialization and final partial step excluded\n";
        if (summary.sampled) {
            output << "GPU attribution: sampled step spans";
            if (summary.unavailable_samples || summary.gpu_attribution_complete == false)
                output << " (partial timing coverage)";
            output << '\n';
        } else if (summary.sampling_attempted)
            output << "GPU stage sampling: unavailable; no known stage timings\n";
        if (summary.approximate) output << "GPU attribution: approximate command-buffer commit step\n";
        output << "GPU timing attribution: " << (summary.gpu_attribution_complete ?
            (*summary.gpu_attribution_complete ? "complete" : "incomplete") : "metadata unavailable") << '\n';
        output << "GPU leaf attribution: " << (summary.gpu_leaf_attribution_complete ?
            (*summary.gpu_leaf_attribution_complete ? "complete" : "coarse or unavailable") : "metadata unavailable") << '\n';
        std::map<std::pair<std::string, bool>, U64> gap_counts;
        for (const auto& gap : summary.sampling_gaps) ++gap_counts[{gap.reason, gap.timing_incomplete}];
        for (const auto& [gap, count] : gap_counts)
            output << "GPU sampling gap: " << gap.first << " count=" << count
                << (gap.second ? " (timing incomplete)\n" : " (coarse leaf attribution)\n");
        if (!summary.complete) output << "Timeline incomplete: invalid=" << summary.invalid_drops
            << " storage=" << summary.storage_drops << '\n';
    } catch (const std::exception& error) { errors << "Step measurements unavailable: " << error.what() << '\n'; }
    try {
        const auto summary = read_profile_report(read_file(paths.trace));
        output << "Profiler: " << (summary.complete ? "complete" : "incomplete")
            << " invalid=" << summary.invalid_drops << " storage=" << summary.storage_drops
            << " open=" << summary.open_ranges << '\n';
        output << "Configuration: split requested=" << (summary.split_requested ? "yes" : "no");
        if (summary.capture_step) output << " capture step=" << *summary.capture_step;
        output << "\nDevice instrumentation: " << summary.device_instrumentation << '\n';
        if (summary.capture) {
            const auto& capture = *summary.capture;
            const auto known = [&](const std::optional<bool>& value) {
                return capture.snapshot_complete && value ? (*value ? "yes" : "no") : "unknown";
            };
            output << "Capture: " << (capture.snapshot_complete ? capture.state : "status incomplete")
                << " step=" << capture.step << " exact Core step=" << known(capture.exact_core_step)
                << " external queues verified=" << known(capture.external_queues_verified) << '\n';
            if (!capture.output_path.empty())
                output << "Capture artifact: " << capture.output_path
                    << " exists=" << known(capture.artifact_exists) << '\n';
            if (capture.snapshot_complete && capture.extra_commits)
                output << "Capture additional command buffers: " << *capture.extra_commits << '\n';
        } else if (summary.capture_step) output << "Capture status unavailable\n";
        output << "Host bytes: inclusive event bytes\n";
        if (summary.typed_gpu) {
            U64 calls{}, known{}, unknown{}, ns{}, bytes{};
            bool saturated = false;
            const auto sum = [&saturated](U64& total, U64 amount) {
                if (!add(total, amount)) { total = std::numeric_limits<U64>::max(); saturated = true; }
            };
            for (const auto& row : summary.rows) {
                sum(calls, row.container_calls); sum(known, row.container_gpu_known_calls);
                sum(unknown, row.container_gpu_unknown_calls); sum(ns, row.container_gpu_ns);
                sum(bytes, row.container_bytes); saturated |= row.container_overflow;
            }
            output << "GPU containers (enclosing spans): calls=" << calls << " timed=" << known
                << " unknown=" << unknown << " time=" << micros(ns) << " bytes=" << bytes;
            if (saturated) output << " saturated";
            output << '\n';
        }
        top(summary, options.top, false, output); top(summary, options.top, true, output);
    } catch (const std::exception& error) { errors << "Profile measurements unavailable: " << error.what() << '\n'; }
    if (options.roofline) output << "Roofline unavailable: Phase 3 per-kernel cost data is absent\n";
    return status;
}
} // namespace quidra::cli
