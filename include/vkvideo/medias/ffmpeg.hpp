#pragma once

#include "vkvideo/core/types.hpp"

#include <iostream>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <memory>
namespace vkvideo::ffmpeg {

inline int av_call(int err) {
  if (err < 0) {
    throw std::runtime_error(av_err2str(err));
  }

  return err;
}

inline void av_call_noexcept(int err) {
  if (err < 0) {
    std::cerr << "FFmpeg error: " << av_err2str(err) << std::endl;
  }
}

class Instance {
public:
  Instance() {
    av_call(avformat_network_init());
    av_log_set_level(AV_LOG_ERROR);
  }

  ~Instance() { av_call_noexcept(avformat_network_deinit()); }

  Instance(const Instance &) = delete;
  Instance &operator=(const Instance &) = delete;

  Instance(Instance &&other) : init{std::exchange(other.init, false)} {}
  Instance &operator=(Instance &&rhs) {
    using std::swap;
    swap(init, rhs.init);
    return *this;
  }

private:
  bool init;
};

enum class MediaType {
  Video = AVMEDIA_TYPE_VIDEO,
  Audio = AVMEDIA_TYPE_AUDIO,
  Subtitle = AVMEDIA_TYPE_SUBTITLE,
};

namespace detail {
struct BufferDeleter {
  void operator()(AVBufferRef *ref) { av_buffer_unref(&ref); }
};

struct PacketDeleter {
  void operator()(AVPacket *packet) { av_packet_free(&packet); }
};

struct FrameDeleter {
  void operator()(AVFrame *frame) { av_frame_free(&frame); }
};

struct InputFormatContextDeleter {
  void operator()(AVFormatContext *context) { avformat_close_input(&context); }
};

struct OutputFormatContextDeleter {
  void operator()(AVFormatContext *context) { avformat_free_context(context); }
};

struct CodecContextDeleter {
  void operator()(AVCodecContext *context) { avcodec_free_context(&context); }
};

struct AVIOContextDeleter {
  void operator()(AVIOContext *context) { avio_closep(&context); }
};

struct SwsContextDeleter {
  void operator()(SwsContext *context) { sws_freeContext(context); }
};

struct SwrContextDeleter {
  void operator()(SwrContext *context) { swr_free(&context); }
};
} // namespace detail

class BufferRef : public std::unique_ptr<AVBufferRef, detail::BufferDeleter> {
public:
  using std::unique_ptr<AVBufferRef, detail::BufferDeleter>::unique_ptr;

  static BufferRef alloc(std::size_t size) {
    return BufferRef{av_buffer_alloc(size)};
  }
};

class Packet : public std::unique_ptr<AVPacket, detail::PacketDeleter> {
public:
  using std::unique_ptr<AVPacket, detail::PacketDeleter>::unique_ptr;

  Packet(Packet &&other)
      : std::unique_ptr<AVPacket, detail::PacketDeleter>{std::move(other)} {}
  Packet &operator=(Packet &&other) {
    std::unique_ptr<AVPacket, detail::PacketDeleter>::operator=(
        std::move(other));
    return *this;
  }

  static Packet create() { return Packet{av_packet_alloc()}; }
};

class Frame : public std::unique_ptr<AVFrame, detail::FrameDeleter> {
public:
  using std::unique_ptr<AVFrame, detail::FrameDeleter>::unique_ptr;

  Frame(Frame &&other)
      : std::unique_ptr<AVFrame, detail::FrameDeleter>{std::move(other)} {}
  Frame &operator=(Frame &&other) {
    std::unique_ptr<AVFrame, detail::FrameDeleter>::operator=(std::move(other));
    return *this;
  }

  static Frame create() { return Frame{av_frame_alloc()}; }

  void ref_to(const Frame &other) { av_frame_ref(get(), other.get()); }
};

template <class Deleter>
class FormatContext : public std::unique_ptr<AVFormatContext, Deleter> {
public:
  using std::unique_ptr<AVFormatContext, Deleter>::unique_ptr;
  virtual ~FormatContext() = default;

  FormatContext(FormatContext &&other)
      : std::unique_ptr<AVFormatContext, Deleter>{std::move(other)} {}
  FormatContext &operator=(FormatContext &&other) {
    std::unique_ptr<AVFormatContext, Deleter>::operator=(std::move(other));
    return *this;
  }

  std::optional<i32> find_best_stream(MediaType type) const {
    int stream_idx = av_find_best_stream(*this, static_cast<AVMediaType>(type),
                                         -1, -1, nullptr, 0);
    if (stream_idx < 0) {
      return std::nullopt;
    }

    return stream_idx;
  }
};

enum class RecvError {
  eSuccess,
  eAgain,
  eEof,
};

class InputFormatContext
    : public FormatContext<detail::InputFormatContextDeleter> {
public:
  using FormatContext<detail::InputFormatContextDeleter>::FormatContext;

  InputFormatContext(InputFormatContext &&other)
      : FormatContext{std::move(other)} {}
  InputFormatContext &operator=(InputFormatContext &&other) {
    FormatContext::operator=(std::move(other));
    return *this;
  }

  static InputFormatContext open(std::string_view path) {
    AVFormatContext *fctx = nullptr;
    av_call(avformat_open_input(&fctx, path.data(), nullptr, nullptr));
    return InputFormatContext{fctx};
  }

  void find_stream_info() {
    av_call(avformat_find_stream_info(get(), nullptr));
  }

  std::pair<Packet, RecvError> read_packet(Packet &&packet = nullptr) {
    // allocate the packet if needed
    if (!packet)
      packet = Packet::create();

    int err = av_read_frame(get(), packet.get());
    RecvError result = RecvError::eSuccess;
    if (err == AVERROR(EAGAIN))
      result = RecvError::eAgain;
    else if (err == AVERROR_EOF)
      result = RecvError::eEof;
    else
      av_call(err);

    return std::pair{std::move(packet), result};
  }
};

class OutputFormatContext
    : public FormatContext<detail::OutputFormatContextDeleter> {
public:
  using FormatContext<detail::OutputFormatContextDeleter>::FormatContext;

  OutputFormatContext(OutputFormatContext &&other)
      : FormatContext{std::move(other)} {}
  OutputFormatContext &operator=(OutputFormatContext &&other) {
    FormatContext::operator=(std::move(other));
    return *this;
  }

  static OutputFormatContext create(std::string_view path) {
    AVFormatContext *fctx = nullptr;
    av_call(
        avformat_alloc_output_context2(&fctx, nullptr, nullptr, path.data()));
    return OutputFormatContext{fctx};
  }

  AVStream *add_stream(const AVCodec *codec) {
    return avformat_new_stream(get(), codec);
  }

  void open_file_if_needed() {
    if (!(get()->oformat->flags & AVFMT_NOFILE)) {
      AVIOContext *avio_ptr = nullptr;
      av_call(avio_open(&avio_ptr, get()->url, AVIO_FLAG_WRITE));
      get()->pb = avio_ptr;
      avio.reset(avio_ptr);
    }
  }

  void begin() {
    open_file_if_needed();
    av_call(avformat_write_header(get(), nullptr));
  }

  void write_packet_interleaved(const Packet &packet) {
    av_call(av_interleaved_write_frame(get(), packet.get()));
  }

  void end() { av_call(av_write_trailer(get())); }

private:
  std::unique_ptr<AVIOContext, detail::AVIOContextDeleter> avio;
};

using OutputFormat = const AVOutputFormat *;
using Codec = const AVCodec *;

inline OutputFormat guess_output_format(std::string_view short_name = {},
                                        std::string_view filename = {},
                                        std::string_view mime_type = {}) {
  return av_guess_format(short_name.data(), filename.data(), mime_type.data());
}

inline Codec find_enc_codec(AVCodecID id) { return avcodec_find_encoder(id); }
inline Codec find_dec_codec(AVCodecID id) { return avcodec_find_decoder(id); }

class CodecContext
    : public std::unique_ptr<AVCodecContext, detail::CodecContextDeleter> {
public:
  using std::unique_ptr<AVCodecContext,
                        detail::CodecContextDeleter>::unique_ptr;

  CodecContext(CodecContext &&other) : unique_ptr{std::move(other)} {}
  CodecContext &operator=(CodecContext &&other) {
    unique_ptr::operator=(std::move(other));
    return *this;
  }

  static CodecContext create(Codec codec) {
    return CodecContext{avcodec_alloc_context3(codec)};
  }

  void open() { av_call(avcodec_open2(get(), nullptr, nullptr)); }

  void copy_params_from(const AVCodecParameters *params) {
    av_call(avcodec_parameters_to_context(get(), params));
  }

  void copy_params_to(AVCodecParameters *params) {
    av_call(avcodec_parameters_from_context(params, get()));
  }

  // DECODE
  void send_packet(const Packet &packet) {
    av_call(avcodec_send_packet(get(), packet.get()));
  }

  std::pair<Frame, RecvError> recv_frame(Frame &&frame = nullptr) {
    // allocate the frame if needed
    if (!frame)
      frame = Frame::create();

    int err = avcodec_receive_frame(get(), frame.get());
    RecvError result = RecvError::eSuccess;
    if (err == AVERROR(EAGAIN)) {
      result = RecvError::eAgain;
    } else if (err == AVERROR_EOF) {
      result = RecvError::eEof;
    } else {
      av_call(err);
    }

    return std::pair{std::move(frame), result};
  }

  // ENCODE
  void send_frame(const Frame &frame) {
    av_call(avcodec_send_frame(get(), frame.get()));
  }

  std::pair<Packet, RecvError> recv_packet(Packet &&packet = nullptr) {
    // allocate the packet if needed
    if (!packet)
      packet = Packet::create();

    int err = avcodec_receive_packet(get(), packet.get());
    RecvError result = RecvError::eSuccess;
    if (err == AVERROR(EAGAIN)) {
      result = RecvError::eAgain;
    } else if (err == AVERROR_EOF) {
      result = RecvError::eEof;
    } else {
      av_call(err);
    }

    return std::pair{std::move(packet), result};
  };
};

using Rational = AVRational;
using PixelFormat = AVPixelFormat;
using SampleFormat = AVSampleFormat;

class ChannelLayout {
public:
  ChannelLayout(AVChannelLayout layout = {}) : layout{layout} {}

  ChannelLayout(const ChannelLayout &other) {
    av_call(av_channel_layout_copy(&layout, &other.layout));
  }

  ChannelLayout &operator=(const ChannelLayout &other) {
    av_call(av_channel_layout_copy(&layout, &other.layout));
    return *this;
  }

  ChannelLayout(ChannelLayout &&other)
      : layout{std::exchange(other.layout, {})} {}

  ChannelLayout &operator=(ChannelLayout &&other) {
    using std::swap;
    swap(layout, other.layout);
    return *this;
  }

  AVChannelLayout *operator->() { return &layout; }
  AVChannelLayout *get() { return &layout; }

private:
  AVChannelLayout layout;
};

inline ChannelLayout ch_layout_mono{AVChannelLayout AV_CHANNEL_LAYOUT_MONO};
inline ChannelLayout ch_layout_stereo{AVChannelLayout AV_CHANNEL_LAYOUT_STEREO};

class VideoRescaler : std::unique_ptr<SwsContext, detail::SwsContextDeleter> {
public:
  using std::unique_ptr<SwsContext, detail::SwsContextDeleter>::unique_ptr;

  void rescale(Frame &dst, const Frame &src) {
    av_call(sws_scale_frame(get(), dst.get(), src.get()));
  }

  void auto_rescale(Frame &dst, const Frame &src) {
    reset(sws_getCachedContext(release(), src->width, src->height,
                               static_cast<AVPixelFormat>(src->format),
                               dst->width, dst->height,
                               static_cast<AVPixelFormat>(dst->format),
                               SWS_BILINEAR, nullptr, nullptr, nullptr));
    rescale(dst, src);
  }
};

} // namespace vkvideo::ffmpeg
