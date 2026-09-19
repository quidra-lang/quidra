#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

extern "C" void* quidra_managed_alloc(unsigned long long bytes);
extern "C" char* quidra_runtime_copy_text(
    const char* data, unsigned long long size);
extern "C" void* quidra_tensor_from_chw(
    const void* data, int dtype,
    unsigned long long channels,
    unsigned long long height,
    unsigned long long width);

namespace {

constexpr int dtype_uint8 = 5;

struct VideoReaderState {
    AVFormatContext* format{};
    AVCodecContext* codec{};
    AVPacket* packet{};
    AVFrame* frame{};
    SwsContext* scaler{};
    int stream{-1};
    long long width{};
    long long height{};
    double fps{};
    bool input_eof{};
    bool flush_sent{};
    std::mutex mutex;
};

thread_local std::string video_last_error;

std::string ffmpeg_message(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, buffer, sizeof(buffer)) == 0) return buffer;
    return "FFmpeg error " + std::to_string(code);
}

void video_set_error(const std::string& message) {
    video_last_error = message.empty() ? "video operation failed" : message;
}

void destroy_state(VideoReaderState* state) {
    if (!state) return;
    if (state->scaler) sws_freeContext(state->scaler);
    if (state->frame) av_frame_free(&state->frame);
    if (state->packet) av_packet_free(&state->packet);
    if (state->codec) avcodec_free_context(&state->codec);
    if (state->format) avformat_close_input(&state->format);
    delete state;
}

VideoReaderState* state_from_reader(void* reader) {
    if (!reader) return nullptr;
    std::uintptr_t bits = 0;
    std::memcpy(&bits, reader, sizeof(bits));
    return reinterpret_cast<VideoReaderState*>(bits);
}

std::size_t checked_product(std::size_t a, std::size_t b) {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        throw std::overflow_error("video frame size overflow");
    }
    return a * b;
}

void* frame_tensor(VideoReaderState& state) {
    const int width = state.frame->width;
    const int height = state.frame->height;
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("decoded video frame has invalid dimensions");
    }

    const auto pixels = checked_product(
        static_cast<std::size_t>(width), static_cast<std::size_t>(height));
    const auto rgb_bytes = checked_product(pixels, static_cast<std::size_t>(3));
    std::vector<std::uint8_t> rgb(rgb_bytes);
    std::vector<std::uint8_t> chw(rgb_bytes);

    state.scaler = sws_getCachedContext(
        state.scaler,
        width, height, static_cast<AVPixelFormat>(state.frame->format),
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!state.scaler) {
        throw std::runtime_error("cannot create video pixel converter");
    }

    std::uint8_t* destination[4]{rgb.data(), nullptr, nullptr, nullptr};
    int destination_linesize[4]{width * 3, 0, 0, 0};
    const int scaled = sws_scale(
        state.scaler,
        state.frame->data, state.frame->linesize,
        0, height,
        destination, destination_linesize);
    if (scaled != height) {
        throw std::runtime_error("video pixel conversion failed");
    }

    for (std::size_t index = 0; index < pixels; ++index) {
        chw[index] = rgb[index * 3];
        chw[pixels + index] = rgb[index * 3 + 1];
        chw[pixels * 2 + index] = rgb[index * 3 + 2];
    }

    state.width = width;
    state.height = height;
    auto* tensor = quidra_tensor_from_chw(
        chw.data(), dtype_uint8, 3,
        static_cast<unsigned long long>(height),
        static_cast<unsigned long long>(width));
    if (!tensor) {
        throw std::runtime_error("cannot allocate decoded video frame tensor");
    }
    return tensor;
}

int decode_next(VideoReaderState& state, void** output) {
    for (;;) {
        const int received = avcodec_receive_frame(state.codec, state.frame);
        if (received == 0) {
            *output = frame_tensor(state);
            av_frame_unref(state.frame);
            return 1;
        }
        if (received == AVERROR_EOF) return 0;
        if (received != AVERROR(EAGAIN)) {
            throw std::runtime_error(
                "video decode failed: " + ffmpeg_message(received));
        }

        if (state.input_eof) {
            if (!state.flush_sent) {
                const int sent = avcodec_send_packet(state.codec, nullptr);
                if (sent < 0 && sent != AVERROR_EOF) {
                    throw std::runtime_error(
                        "video decoder flush failed: " + ffmpeg_message(sent));
                }
                state.flush_sent = true;
                continue;
            }
            return 0;
        }

        for (;;) {
            const int read = av_read_frame(state.format, state.packet);
            if (read == AVERROR_EOF) {
                state.input_eof = true;
                break;
            }
            if (read < 0) {
                throw std::runtime_error(
                    "video container read failed: " + ffmpeg_message(read));
            }
            if (state.packet->stream_index != state.stream) {
                av_packet_unref(state.packet);
                continue;
            }

            const int sent = avcodec_send_packet(state.codec, state.packet);
            av_packet_unref(state.packet);
            if (sent < 0) {
                throw std::runtime_error(
                    "video packet decode failed: " + ffmpeg_message(sent));
            }
            break;
        }
    }
}

} // namespace

extern "C" void* quidra_video_open_raw(const char* path) {
    video_last_error.clear();
    if (!path || !*path) {
        video_set_error("video path is empty");
        return nullptr;
    }

    VideoReaderState* state = nullptr;
    try {
        state = new VideoReaderState();

        int code = avformat_open_input(&state->format, path, nullptr, nullptr);
        if (code < 0) {
            throw std::runtime_error(
                "cannot open video: " + ffmpeg_message(code));
        }
        code = avformat_find_stream_info(state->format, nullptr);
        if (code < 0) {
            throw std::runtime_error(
                "cannot read video stream metadata: " + ffmpeg_message(code));
        }

        const AVCodec* decoder = nullptr;
        state->stream = av_find_best_stream(
            state->format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (state->stream < 0 || !decoder) {
            throw std::runtime_error("video contains no decodable video stream");
        }

        state->codec = avcodec_alloc_context3(decoder);
        if (!state->codec) throw std::bad_alloc();

        code = avcodec_parameters_to_context(
            state->codec, state->format->streams[state->stream]->codecpar);
        if (code < 0) {
            throw std::runtime_error(
                "cannot configure video decoder: " + ffmpeg_message(code));
        }
        code = avcodec_open2(state->codec, decoder, nullptr);
        if (code < 0) {
            throw std::runtime_error(
                "cannot initialize video decoder: " + ffmpeg_message(code));
        }

        state->packet = av_packet_alloc();
        state->frame = av_frame_alloc();
        if (!state->packet || !state->frame) throw std::bad_alloc();

        state->width = state->codec->width;
        state->height = state->codec->height;
        if (state->width <= 0 || state->height <= 0) {
            throw std::runtime_error("video has invalid dimensions");
        }

        AVStream* stream = state->format->streams[state->stream];
        AVRational rate = av_guess_frame_rate(state->format, stream, nullptr);
        if (rate.num <= 0 || rate.den <= 0) rate = stream->avg_frame_rate;
        if ((rate.num <= 0 || rate.den <= 0) &&
            state->codec->framerate.num > 0 && state->codec->framerate.den > 0) {
            rate = state->codec->framerate;
        }
        state->fps =
            (rate.num > 0 && rate.den > 0) ? av_q2d(rate) : 0.0;
        if (!(state->fps > 0.0)) {
            throw std::runtime_error("video frame rate is unavailable");
        }

        auto* wrapper = static_cast<unsigned char*>(quidra_managed_alloc(8));
        const auto bits = reinterpret_cast<std::uintptr_t>(state);
        std::memcpy(wrapper, &bits, sizeof(bits));
        return wrapper;
    } catch (const std::exception& error) {
        destroy_state(state);
        video_set_error(error.what());
        return nullptr;
    } catch (...) {
        destroy_state(state);
        video_set_error("video operation failed");
        return nullptr;
    }
}

extern "C" int quidra_video_read(void* reader, void** output) {
    video_last_error.clear();
    if (output) *output = nullptr;
    auto* state = state_from_reader(reader);
    if (!state || !output) {
        video_set_error("invalid video Reader");
        return -1;
    }

    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        return decode_next(*state, output);
    } catch (const std::exception& error) {
        if (state->frame) av_frame_unref(state->frame);
        if (state->packet) av_packet_unref(state->packet);
        video_set_error(error.what());
        return -1;
    } catch (...) {
        if (state->frame) av_frame_unref(state->frame);
        if (state->packet) av_packet_unref(state->packet);
        video_set_error("video decode failed");
        return -1;
    }
}

extern "C" long long quidra_video_width(void* reader) {
    auto* state = state_from_reader(reader);
    if (!state) return 0;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->width;
}

extern "C" long long quidra_video_height(void* reader) {
    auto* state = state_from_reader(reader);
    if (!state) return 0;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->height;
}

extern "C" double quidra_video_fps(void* reader) {
    auto* state = state_from_reader(reader);
    if (!state) return 0.0;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->fps;
}

extern "C" char* quidra_video_last_error_copy() {
    const auto& message =
        video_last_error.empty() ? std::string("video operation failed")
                                 : video_last_error;
    return quidra_runtime_copy_text(
        message.data(), static_cast<unsigned long long>(message.size()));
}

extern "C" void quidra_video_reader_drop(void* reader) {
    if (!reader) return;
    auto* state = state_from_reader(reader);
    std::uintptr_t zero = 0;
    std::memcpy(reader, &zero, sizeof(zero));
    destroy_state(state);
}
