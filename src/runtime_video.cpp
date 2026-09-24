#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "runtime_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

extern "C" char* quidra_runtime_copy_text(
    const char* data, unsigned long long size);
extern "C" void* quidra_tensor_from_chw(
    const void* data, int dtype,
    unsigned long long channels,
    unsigned long long height,
    unsigned long long width);

namespace {

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

thread_local std::string video_last_error;

void set_error(const std::string& message) {
    video_last_error = message.empty() ? "video operation failed" : message;
}

std::string ffmpeg_message(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, buffer, sizeof(buffer)) == 0) return buffer;
    return "FFmpeg error " + std::to_string(code);
}

struct VideoResource {
    std::string label;
    std::ifstream stream;
    std::mutex mutex;
    std::int64_t size{};

    explicit VideoResource(std::string source) : label(std::move(source)) {
        stream.open(label, std::ios::binary);
        if (!stream) throw std::runtime_error("cannot open video");
        stream.seekg(0, std::ios::end);
        const auto end = stream.tellg();
        if (end == std::streampos(-1)) {
            throw std::runtime_error("cannot determine video size");
        }
        const auto length = end - std::streampos(0);
        if (length < 0 ||
            length > static_cast<std::streamoff>(
                std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error("video size is not representable");
        }
        size = static_cast<std::int64_t>(length);
        stream.clear();
        stream.seekg(0, std::ios::beg);
        if (!stream) throw std::runtime_error("cannot seek video source");
    }
};

struct AvioState {
    std::shared_ptr<VideoResource> resource;
    std::int64_t position{};
};

int video_read_packet(void* opaque, std::uint8_t* buffer, int requested) {
    auto* state = static_cast<AvioState*>(opaque);
    if (!state || !state->resource || !buffer || requested <= 0) {
        return AVERROR(EINVAL);
    }
    if (state->position >= state->resource->size) return AVERROR_EOF;
    const auto remaining = state->resource->size - state->position;
    const auto amount = static_cast<std::streamsize>(
        std::min<std::int64_t>(remaining, requested));

    std::lock_guard<std::mutex> lock(state->resource->mutex);
    auto& stream = state->resource->stream;
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(state->position), std::ios::beg);
    if (!stream) return AVERROR(EIO);
    stream.read(reinterpret_cast<char*>(buffer), amount);
    const auto received = stream.gcount();
    if (received > 0) {
        state->position += static_cast<std::int64_t>(received);
        return static_cast<int>(received);
    }
    // The opened resource recorded its size when video.open succeeded. EOF
    // before that boundary is a truncated/failed source, not clean stream end.
    return AVERROR(EIO);
}

std::int64_t video_seek(void* opaque, std::int64_t offset, int whence) {
    auto* state = static_cast<AvioState*>(opaque);
    if (!state || !state->resource) return AVERROR(EINVAL);
    if (whence & AVSEEK_SIZE) return state->resource->size;
    const int origin = whence & ~AVSEEK_FORCE;

    std::int64_t base = 0;
    if (origin == SEEK_SET) {
        base = 0;
    } else if (origin == SEEK_CUR) {
        base = state->position;
    } else if (origin == SEEK_END) {
        base = state->resource->size;
    } else {
        return AVERROR(EINVAL);
    }

    if ((offset > 0 &&
         base > std::numeric_limits<std::int64_t>::max() - offset) ||
        (offset < 0 &&
         base < std::numeric_limits<std::int64_t>::min() - offset)) {
        return AVERROR(EINVAL);
    }
    const auto target = base + offset;
    if (target < 0 || target > state->resource->size) return AVERROR(EINVAL);
    state->position = target;
    return target;
}

struct Session {
    AVFormatContext* format{};
    AVIOContext* io{};
    AvioState* io_state{};
    AVCodecContext* codec{};
    AVPacket* packet{};
    AVFrame* frame{};
    SwsContext* scaler{};
    int stream{-1};
    bool draining{};

    ~Session() {
        if (scaler) sws_freeContext(scaler);
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (codec) avcodec_free_context(&codec);
        if (format) avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        delete io_state;
    }
};

std::unique_ptr<Session> open_session(
    const std::shared_ptr<VideoResource>& resource) {
    if (!resource) throw std::invalid_argument("invalid video source");
    auto session = std::make_unique<Session>();
    session->io_state = new AvioState{resource, 0};
    constexpr int io_buffer_size = 64 * 1024;
    auto* io_buffer = static_cast<unsigned char*>(
        av_malloc(static_cast<std::size_t>(io_buffer_size)));
    if (!io_buffer) throw std::bad_alloc();
    session->io = avio_alloc_context(
        io_buffer, io_buffer_size, 0, session->io_state,
        video_read_packet, nullptr, video_seek);
    if (!session->io) {
        av_free(io_buffer);
        throw std::bad_alloc();
    }
    session->format = avformat_alloc_context();
    if (!session->format) throw std::bad_alloc();
    session->format->pb = session->io;
    session->format->flags |= AVFMT_FLAG_CUSTOM_IO;

    int code = avformat_open_input(
        &session->format, resource->label.c_str(), nullptr, nullptr);
    if (code < 0) {
        throw std::runtime_error("cannot open video: " + ffmpeg_message(code));
    }
    code = avformat_find_stream_info(session->format, nullptr);
    if (code < 0) {
        throw std::runtime_error(
            "cannot read video stream metadata: " + ffmpeg_message(code));
    }

#if LIBAVFORMAT_VERSION_MAJOR >= 59
    const AVCodec* decoder = nullptr;
    session->stream = av_find_best_stream(
        session->format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
#else
    // FFmpeg 4.4 / libavformat 58 predates the const-correct decoder_ret
    // signature introduced by FFmpeg 5. Keep one source compatible with both.
    AVCodec* legacy_decoder = nullptr;
    session->stream = av_find_best_stream(
        session->format, AVMEDIA_TYPE_VIDEO, -1, -1, &legacy_decoder, 0);
    const AVCodec* decoder = legacy_decoder;
#endif
    if (session->stream < 0 || !decoder) {
        throw std::runtime_error("video contains no decodable video stream");
    }

    session->codec = avcodec_alloc_context3(decoder);
    if (!session->codec) throw std::bad_alloc();
    code = avcodec_parameters_to_context(
        session->codec, session->format->streams[session->stream]->codecpar);
    if (code < 0) {
        throw std::runtime_error(
            "cannot configure video decoder: " + ffmpeg_message(code));
    }
    code = avcodec_open2(session->codec, decoder, nullptr);
    if (code < 0) {
        throw std::runtime_error(
            "cannot initialize video decoder: " + ffmpeg_message(code));
    }

    session->packet = av_packet_alloc();
    session->frame = av_frame_alloc();
    if (!session->packet || !session->frame) throw std::bad_alloc();
    return session;
}

int decode_next(Session& session) {
    for (;;) {
        const int received = avcodec_receive_frame(session.codec, session.frame);
        if (received == 0) return 1;
        if (received == AVERROR_EOF) return 0;
        if (received != AVERROR(EAGAIN)) {
            throw std::runtime_error(
                "video decode failed: " + ffmpeg_message(received));
        }

        bool supplied = false;
        while (!supplied) {
            const int read = av_read_frame(session.format, session.packet);
            if (read == AVERROR_EOF) {
                if (!session.draining) {
                    const int sent = avcodec_send_packet(session.codec, nullptr);
                    if (sent < 0 && sent != AVERROR_EOF) {
                        throw std::runtime_error(
                            "video decoder flush failed: " + ffmpeg_message(sent));
                    }
                    session.draining = true;
                    supplied = true;
                    continue;
                }
                return 0;
            }
            if (read < 0) {
                throw std::runtime_error(
                    "video demux read failed: " + ffmpeg_message(read));
            }
            if (session.packet->stream_index != session.stream) {
                av_packet_unref(session.packet);
                continue;
            }
            const int sent = avcodec_send_packet(session.codec, session.packet);
            av_packet_unref(session.packet);
            if (sent == AVERROR(EAGAIN)) {
                throw std::runtime_error(
                    "video decoder packet backpressure invariant failed");
            }
            if (sent < 0) {
                throw std::runtime_error(
                    "video packet decode failed: " + ffmpeg_message(sent));
            }
            supplied = true;
        }
    }
}

struct Source {
    std::shared_ptr<VideoResource> resource;
    long long width{};
    long long height{};
    double fps{-1.0};
    long long frames{-1};
    double duration{-1.0};
};

struct Reader {
    std::shared_ptr<Source> source;
    long long position{};
    std::unique_ptr<Session> session;
};

Reader* reader_from_value(void* value) {
    if (!value) return nullptr;
    std::uintptr_t bits{};
    std::memcpy(&bits, value, sizeof(bits));
    return reinterpret_cast<Reader*>(bits);
}

void* make_value(Reader* reader) {
    auto* value = quidra_managed_alloc(sizeof(std::uintptr_t));
    const auto bits = reinterpret_cast<std::uintptr_t>(reader);
    std::memcpy(value, &bits, sizeof(bits));
    return value;
}

std::shared_ptr<Source> describe(
    std::shared_ptr<VideoResource> resource, const Session& session) {
    auto source = std::make_shared<Source>();
    source->resource = std::move(resource);
    source->width = session.codec->width;
    source->height = session.codec->height;
    if (source->width <= 0 || source->height <= 0) {
        throw std::runtime_error("video has invalid dimensions");
    }

    AVStream* stream = session.format->streams[session.stream];
    AVRational rate = stream->avg_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) rate = stream->r_frame_rate;
    if (rate.num > 0 && rate.den > 0) {
        const double value = av_q2d(rate);
        if (std::isfinite(value) && value > 0.0) source->fps = value;
    }
    if (stream->nb_frames > 0) {
        source->frames = static_cast<long long>(stream->nb_frames);
    }
    if (stream->duration != AV_NOPTS_VALUE && stream->duration >= 0) {
        const double value =
            static_cast<double>(stream->duration) * av_q2d(stream->time_base);
        if (std::isfinite(value) && value >= 0.0) source->duration = value;
    } else if (session.format->duration != AV_NOPTS_VALUE &&
               session.format->duration >= 0) {
        const double value =
            static_cast<double>(session.format->duration) /
            static_cast<double>(AV_TIME_BASE);
        if (std::isfinite(value) && value >= 0.0) source->duration = value;
    }
    return source;
}

void advance_to(Session& session, long long frame) {
    for (long long index = 0; index < frame; ++index) {
        av_frame_unref(session.frame);
        if (decode_next(session) != 1) {
            throw std::out_of_range("video seek is beyond end of stream");
        }
    }
}

void ensure_session(Reader& reader) {
    if (reader.session) return;
    auto session = open_session(reader.source->resource);
    advance_to(*session, reader.position);
    reader.session = std::move(session);
}

std::size_t checked_product(std::size_t a, std::size_t b) {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        throw std::overflow_error("video frame size overflow");
    }
    return a * b;
}

int component_depth(const AVFrame& frame) {
    const auto* descriptor =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame.format));
    if (!descriptor) return 8;
    int depth = 0;
    for (int index = 0; index < descriptor->nb_components; ++index) {
        depth = std::max(depth, static_cast<int>(descriptor->comp[index].depth));
    }
    return depth > 0 ? depth : 8;
}

template <typename SourceSample, typename TargetSample>
std::vector<std::uint8_t> convert_samples(
    const std::vector<std::uint8_t>& input) {
    if (input.size() % sizeof(SourceSample) != 0) {
        throw std::runtime_error("invalid decoded video sample storage");
    }
    const std::size_t count = input.size() / sizeof(SourceSample);
    std::vector<std::uint8_t> output(count * sizeof(TargetSample));
    for (std::size_t index = 0; index < count; ++index) {
        SourceSample value{};
        std::memcpy(
            &value, input.data() + index * sizeof(SourceSample),
            sizeof(SourceSample));
        if constexpr (std::is_integral_v<TargetSample>) {
            if (static_cast<unsigned long long>(value) >
                static_cast<unsigned long long>(
                    std::numeric_limits<TargetSample>::max())) {
                throw std::range_error(
                    "video dtype conversion would change an integer value");
            }
        }
        const TargetSample converted = static_cast<TargetSample>(value);
        std::memcpy(
            output.data() + index * sizeof(TargetSample),
            &converted, sizeof(TargetSample));
    }
    return output;
}

template <typename SourceSample>
std::vector<std::uint8_t> convert_to(
    const std::vector<std::uint8_t>& input, int dtype) {
    switch (dtype) {
        case dtype_int8: return convert_samples<SourceSample, std::int8_t>(input);
        case dtype_int16: return convert_samples<SourceSample, std::int16_t>(input);
        case dtype_int32: return convert_samples<SourceSample, std::int32_t>(input);
        case dtype_int64: return convert_samples<SourceSample, std::int64_t>(input);
        case dtype_uint8: return convert_samples<SourceSample, std::uint8_t>(input);
        case dtype_uint16: return convert_samples<SourceSample, std::uint16_t>(input);
        case dtype_uint32: return convert_samples<SourceSample, std::uint32_t>(input);
        case dtype_uint64: return convert_samples<SourceSample, std::uint64_t>(input);
        case dtype_float32: return convert_samples<SourceSample, float>(input);
        case dtype_float64: return convert_samples<SourceSample, double>(input);
        default: throw std::invalid_argument("unsupported video target dtype");
    }
}

std::vector<std::uint8_t> convert_dtype(
    const std::vector<std::uint8_t>& input, int source_dtype, int target_dtype) {
    if (source_dtype == target_dtype) return input;
    if (source_dtype == dtype_uint8) return convert_to<std::uint8_t>(input, target_dtype);
    if (source_dtype == dtype_uint16) return convert_to<std::uint16_t>(input, target_dtype);
    throw std::invalid_argument("unsupported decoded video dtype");
}

void require_extent(long long actual, long long expected, const char* axis) {
    if (expected < 0) return;
    if (actual != expected) {
        throw std::invalid_argument(
            std::string("video ") + axis +
            " does not match the expected tensor shape constraint");
    }
}

void* frame_tensor(
    Reader& reader, int expected_dtype, int target_dtype,
    long long target_channels, long long expected_channels,
    long long expected_height, long long expected_width, int* output_dtype) {
    AVFrame& frame = *reader.session->frame;
    if (frame.width != reader.source->width ||
        frame.height != reader.source->height) {
        throw std::runtime_error(
            "dynamic-resolution video is not supported by video.Reader");
    }

    const long long channels = target_channels == 0 ? 3 : target_channels;
    if (channels != 1 && channels != 3 && channels != 4) {
        throw std::invalid_argument("video channel must evaluate to 1, 3, or 4");
    }
    const int depth = component_depth(frame);
    if (depth > 16) {
        throw std::invalid_argument(
            "video component precision above 16 bits is not supported");
    }
    const int source_dtype = depth <= 8 ? dtype_uint8 : dtype_uint16;
    const std::size_t sample_bytes = source_dtype == dtype_uint8 ? 1U : 2U;
    const AVPixelFormat output_format =
        sample_bytes == 1
            ? (channels == 1 ? AV_PIX_FMT_GRAY8
                             : channels == 3 ? AV_PIX_FMT_RGB24
                                             : AV_PIX_FMT_RGBA)
            : (channels == 1 ? AV_PIX_FMT_GRAY16
                             : channels == 3 ? AV_PIX_FMT_RGB48
                                             : AV_PIX_FMT_RGBA64);

    reader.session->scaler = sws_getCachedContext(
        reader.session->scaler,
        frame.width, frame.height, static_cast<AVPixelFormat>(frame.format),
        frame.width, frame.height, output_format,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!reader.session->scaler) {
        throw std::runtime_error("cannot create video pixel converter");
    }

    const auto width = static_cast<std::size_t>(frame.width);
    const auto height = static_cast<std::size_t>(frame.height);
    const auto pixels = checked_product(width, height);
    const auto samples =
        checked_product(pixels, static_cast<std::size_t>(channels));
    const auto bytes = checked_product(samples, sample_bytes);
    const auto row_bytes = checked_product(
        checked_product(width, static_cast<std::size_t>(channels)), sample_bytes);
    if (row_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::overflow_error("video row size exceeds decoder limits");
    }

    // libswscale's optimized writers may require line-end padding beyond the
    // visible packed row. Let FFmpeg allocate an aligned destination image
    // instead of pointing it at a tightly-sized std::vector.
    std::uint8_t* destination[4]{nullptr, nullptr, nullptr, nullptr};
    int destination_linesize[4]{0, 0, 0, 0};
    const int allocated = av_image_alloc(
        destination, destination_linesize,
        frame.width, frame.height, output_format, 64);
    if (allocated < 0) {
        throw std::runtime_error(
            "cannot allocate video pixel-conversion buffer: " +
            ffmpeg_message(allocated));
    }
    struct ImageBufferGuard {
        std::uint8_t** data{};
        ~ImageBufferGuard() {
            if (data && data[0]) av_freep(&data[0]);
        }
    } image_buffer{destination};

    if (destination_linesize[0] < 0 ||
        static_cast<std::size_t>(destination_linesize[0]) < row_bytes) {
        throw std::runtime_error("video pixel-conversion stride is invalid");
    }

    const int scaled = sws_scale(
        reader.session->scaler,
        frame.data, frame.linesize, 0, frame.height,
        destination, destination_linesize);
    if (scaled != frame.height) {
        throw std::runtime_error("video pixel conversion failed");
    }

    std::vector<std::uint8_t> chw(bytes);
    for (std::size_t y = 0; y < height; ++y) {
        const auto* row =
            destination[0] + y * static_cast<std::size_t>(destination_linesize[0]);
        for (std::size_t x = 0; x < width; ++x) {
            for (std::size_t channel = 0;
                 channel < static_cast<std::size_t>(channels); ++channel) {
                const auto source =
                    (x * static_cast<std::size_t>(channels) + channel) *
                    sample_bytes;
                const auto destination_offset =
                    ((channel * height + y) * width + x) * sample_bytes;
                std::memcpy(
                    chw.data() + destination_offset,
                    row + source, sample_bytes);
            }
        }
    }

    const int final_dtype = target_dtype == 0 ? source_dtype : target_dtype;
    if (final_dtype < 1 || final_dtype > 10) {
        throw std::invalid_argument("unsupported video target dtype");
    }
    if (expected_dtype != 0 && expected_dtype != final_dtype) {
        throw std::invalid_argument(
            "video dtype does not match the expected tensor element type");
    }
    require_extent(channels, expected_channels, "channels");
    require_extent(frame.height, expected_height, "height");
    require_extent(frame.width, expected_width, "width");

    auto converted = convert_dtype(chw, source_dtype, final_dtype);
    void* tensor = quidra_tensor_from_chw(
        converted.data(), final_dtype,
        static_cast<unsigned long long>(channels),
        static_cast<unsigned long long>(height),
        static_cast<unsigned long long>(width));
    if (!tensor) {
        throw std::runtime_error("cannot allocate decoded video frame tensor");
    }
    *output_dtype = final_dtype;
    return tensor;
}

} // namespace

extern "C" void* quidra_video_open_raw(const char* path) {
    video_last_error.clear();
    if (!path || !*path) {
        set_error("video path is empty");
        return nullptr;
    }
    try {
        auto resource = std::make_shared<VideoResource>(std::string(path));
        auto session = open_session(resource);
        auto source = describe(std::move(resource), *session);
        auto* reader = new Reader{std::move(source), 0, std::move(session)};
        return make_value(reader);
    } catch (const std::exception& error) {
        set_error(error.what());
        return nullptr;
    } catch (...) {
        set_error("video operation failed");
        return nullptr;
    }
}

extern "C" int quidra_video_read(
    void* value, int expected_dtype, int target_dtype,
    long long target_channels, long long expected_channels,
    long long expected_height, long long expected_width,
    void** output, int* output_dtype) {
    video_last_error.clear();
    if (output) *output = nullptr;
    if (output_dtype) *output_dtype = 0;
    try {
        auto* reader = reader_from_value(value);
        if (!reader || !reader->source || !output || !output_dtype) {
            throw std::invalid_argument("invalid video Reader");
        }
        if (target_dtype < 0 || target_dtype > 10 ||
            expected_dtype < 0 || expected_dtype > 10) {
            throw std::invalid_argument("unsupported video dtype");
        }
        if (target_channels != 0 && target_channels != 1 &&
            target_channels != 3 && target_channels != 4) {
            throw std::invalid_argument(
                "video channel must evaluate to 1, 3, or 4");
        }

        ensure_session(*reader);
        av_frame_unref(reader->session->frame);
        if (decode_next(*reader->session) == 0) return 0;

        *output = frame_tensor(
            *reader, expected_dtype, target_dtype, target_channels,
            expected_channels, expected_height, expected_width, output_dtype);
        ++reader->position;
        av_frame_unref(reader->session->frame);
        return 1;
    } catch (const std::exception& error) {
        if (auto* reader = reader_from_value(value)) reader->session.reset();
        set_error(error.what());
        return -1;
    } catch (...) {
        if (auto* reader = reader_from_value(value)) reader->session.reset();
        set_error("video decode failed");
        return -1;
    }
}

extern "C" long long quidra_video_width(void* value) {
    const auto* reader = reader_from_value(value);
    return reader && reader->source ? reader->source->width : 0;
}

extern "C" long long quidra_video_height(void* value) {
    const auto* reader = reader_from_value(value);
    return reader && reader->source ? reader->source->height : 0;
}

extern "C" double quidra_video_fps(void* value) {
    const auto* reader = reader_from_value(value);
    return reader && reader->source ? reader->source->fps : -1.0;
}

extern "C" long long quidra_video_frames(void* value) {
    const auto* reader = reader_from_value(value);
    return reader && reader->source ? reader->source->frames : -1;
}

extern "C" double quidra_video_duration(void* value) {
    const auto* reader = reader_from_value(value);
    return reader && reader->source ? reader->source->duration : -1.0;
}

extern "C" long long quidra_video_position(void* value) {
    const auto* reader = reader_from_value(value);
    return reader ? reader->position : 0;
}

extern "C" bool quidra_video_seek(void* value, long long frame) {
    video_last_error.clear();
    try {
        auto* reader = reader_from_value(value);
        if (!reader || !reader->source) {
            throw std::invalid_argument("invalid video Reader");
        }
        if (frame < 0) {
            throw std::invalid_argument("video seek frame cannot be negative");
        }
        auto session = open_session(reader->source->resource);
        advance_to(*session, frame);
        reader->session = std::move(session);
        reader->position = frame;
        return true;
    } catch (const std::exception& error) {
        set_error(error.what());
        return false;
    } catch (...) {
        set_error("video seek failed");
        return false;
    }
}

extern "C" char* quidra_video_last_error_copy() {
    const auto& message =
        video_last_error.empty()
            ? std::string("video operation failed")
            : video_last_error;
    return quidra_runtime_copy_text(
        message.data(), static_cast<unsigned long long>(message.size()));
}

extern "C" void* quidra_video_reader_clone(void* value) {
    try {
        auto* reader = reader_from_value(value);
        if (!reader || !reader->source) return nullptr;
        auto* copy = new Reader{reader->source, reader->position, nullptr};
        return make_value(copy);
    } catch (...) {
        std::fprintf(stderr, "Quidra runtime error: allocation failed\n");
        std::exit(101);
    }
}

extern "C" void quidra_video_reader_drop(void* value) {
    if (!value) return;
    auto* reader = reader_from_value(value);
    std::uintptr_t zero{};
    std::memcpy(value, &zero, sizeof(zero));
    delete reader;
}
