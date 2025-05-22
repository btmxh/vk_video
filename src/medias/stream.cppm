module;

#ifdef VKVIDEO_HAVE_WEBP
#include "webp/demux.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

#include <cassert>

export module vkvideo.medias:stream;

import std;
import vulkan_hpp;
import vkvideo.core;
import vkvideo.third_party;

export namespace vkvideo::medias {

class Stream {
public:
  Stream() = default;
  virtual ~Stream() = default;

  virtual std::pair<tp::ffmpeg::Frame, bool>
  next_frame(tp::ffmpeg::Frame &&frame = {}) = 0;
  virtual bool seek(i64 pos) { return false; }
  virtual std::optional<i32> get_num_frames() { return std::nullopt; }
  virtual std::optional<i64> get_duration() { return std::nullopt; }
};

enum class HWAccel {
  eAuto = 0,
  eOff,
  eOn,
};

class FFmpegStream;

class RawFFmpegStream {
public:
  RawFFmpegStream(std::string_view path, tp::ffmpeg::MediaType stream_type) {
    demuxer = tp::ffmpeg::InputFormatContext::open(path);
    demuxer.find_stream_info();
    tp::ffmpeg::av_call(stream_index = av_find_best_stream(
                            demuxer.get(),
                            static_cast<AVMediaType>(stream_type), -1, -1,
                            &codec, 0));
  }

  i32 width() const { return demuxer->streams[stream_index]->codecpar->width; }
  i32 height() const {
    return demuxer->streams[stream_index]->codecpar->height;
  }
  // might not be accurate
  std::optional<i64> est_num_frames() const {
    auto est_num_frames = demuxer->streams[stream_index]->nb_frames;
    if (est_num_frames <= 0)
      return std::nullopt;
    return est_num_frames;
  }

  std::optional<std::size_t> est_vram_bytes() const {
    auto num_frames = est_num_frames();
    if (!num_frames.has_value())
      return std::nullopt;
    auto format = static_cast<tp::ffmpeg::PixelFormat>(
        demuxer->streams[stream_index]->codecpar->format);
    auto pixdesc = tp::ffmpeg::get_pix_fmt_desc(format);
    auto bpp = av_get_padded_bits_per_pixel(pixdesc);
    if (bpp <= 0)
      return std::nullopt;
    return static_cast<std::size_t>(bpp) * width() * height() *
           num_frames.value();
  }

  tp::ffmpeg::InputFormatContext &get_demuxer() { return demuxer; }
  i32 get_stream_index() const { return stream_index; }
  tp::ffmpeg::Codec get_codec() const { return codec; }

  void seek(i64 pos) {
    tp::ffmpeg::av_call(av_seek_frame(demuxer.get(), -1,
                                      pos * AV_TIME_BASE / 1000000000,
                                      AVSEEK_FLAG_BACKWARD));
    reach_eof_packet = false;
  }

  std::pair<tp::ffmpeg::Packet, tp::ffmpeg::RecvError>
  read_packet(tp::ffmpeg::Packet &&packet = nullptr) {
    if (!packet)
      packet = tp::ffmpeg::Packet::create();

    tp::ffmpeg::RecvError err;
    while (true) {
      std::tie(packet, err) = demuxer.read_packet(std::move(packet));
      switch (err) {
      case tp::ffmpeg::RecvError::eSuccess:
        if (packet->stream_index == stream_index)
          return std::make_pair(std::move(packet), err);
        break;
      case tp::ffmpeg::RecvError::eAgain:
        throw std::logic_error{"should not reach here"};
      case tp::ffmpeg::RecvError::eEof:
        bool eof = std::exchange(reach_eof_packet, true);
        return std::make_pair(std::move(packet),
                              eof ? tp::ffmpeg::RecvError::eEof
                                  : tp::ffmpeg::RecvError::eSuccess);
      }
    }
  }

private:
  tp::ffmpeg::InputFormatContext demuxer;
  i32 stream_index;
  tp::ffmpeg::Codec codec;
  bool reach_eof_packet = false;
};

class FFmpegStream : public Stream {
public:
  FFmpegStream(RawFFmpegStream raw, const tp::ffmpeg::BufferRef &hwaccel_ctx,
               tp::ffmpeg::MediaType stream_type = tp::ffmpeg::MediaType::Video,
               HWAccel hwaccel = HWAccel::eAuto)
      : raw{std::move(raw)} {
    decoder = tp::ffmpeg::CodecContext::create(raw.get_codec());
    decoder.copy_params_from(this->raw.get_demuxer()
                                 ->streams[this->raw.get_stream_index()]
                                 ->codecpar);
    if (hwaccel == HWAccel::eOn || hwaccel == HWAccel::eAuto) {
      if (stream_type == tp::ffmpeg::MediaType::Video) {
        decoder->hw_device_ctx = av_buffer_ref(hwaccel_ctx.get());
        decoder->opaque = this;
        decoder->get_format = [](AVCodecContext *c,
                                 const tp::ffmpeg::PixelFormat *formats) {
          for (auto p = formats; *p != tp::ffmpeg::PixelFormat::AV_PIX_FMT_NONE;
               ++p) {
            if (*p == tp::ffmpeg::PixelFormat::AV_PIX_FMT_VULKAN) {
              // Vulkan is available, use it
              return *p;
            }
          }

          std::println("Vulkan-accelerated decoding is not available for this "
                       "format. Falling back to software decoding.");
          // fallback to sw format on vulkan error
          return *formats;
        };
        decoder->pix_fmt = tp::ffmpeg::PixelFormat::AV_PIX_FMT_VULKAN;
      } else {
        std::cerr << "WARNING: Hardware acceleration is only supported for "
                     "video streams\n";
      }
    }

    decoder.open();

    current_packet = tp::ffmpeg::Packet::create();
  }

  ~FFmpegStream() = default;

  std::pair<tp::ffmpeg::Frame, bool>
  next_frame(tp::ffmpeg::Frame &&frame = {}) override {
    auto rescale_pts = [&](i64 &pts) {
      pts = tp::ffmpeg::rescale_to_ns(pts, get_stream().time_base);
    };

    auto read_packet = [&]() {};

    while (true) {
      tp::ffmpeg::RecvError err;
      std::tie(frame, err) = decoder.recv_frame(std::move(frame));
      if (err == tp::ffmpeg::RecvError::eSuccess) {
        rescale_pts(frame->pts);
        rescale_pts(frame->duration);
        return {std::move(frame), true};
      }

      std::tie(current_packet, err) =
          raw.read_packet(std::move(current_packet));
      if (err != tp::ffmpeg::RecvError::eSuccess) {
        return {std::move(frame), false};
      }

      decoder.send_packet(current_packet);
    }
  }

  bool seek(i64 pos) override {
    decoder.flush_buffers();
    raw.seek(pos);
    return true;
  }

  std::optional<i32> get_num_frames() override {
    i32 nb_frames = get_stream().nb_frames;
    if (nb_frames == 0)
      return std::nullopt;
    return nb_frames;
  }

  std::optional<i64> get_duration() override {
    // TODO: this is not entirely accurate
    return tp::ffmpeg::rescale_to_ns(get_stream().duration,
                                     get_stream().time_base);
  }

  tp::ffmpeg::InputFormatContext &get_demuxer() { return raw.get_demuxer(); }
  tp::ffmpeg::CodecContext &get_decoder() { return decoder; }

private:
  RawFFmpegStream raw;
  tp::ffmpeg::CodecContext decoder;
  tp::ffmpeg::Packet current_packet;

  AVStream &get_stream() {
    return *raw.get_demuxer()->streams[raw.get_stream_index()];
  }
};

#ifdef VKVIDEO_HAVE_WEBP
class AnimWebPStream : public Stream {
public:
  AnimWebPStream(std::vector<char> data)
      : data{std::move(data)},
        decoder{std::span<const u8>{reinterpret_cast<const u8 *>(data.data()),
                                    data.size()}} {}
  ~AnimWebPStream() = default;

  std::pair<tp::ffmpeg::Frame, bool>
  next_frame(tp::ffmpeg::Frame &&frame = {}) override {
    if (!decoder.has_more_frames()) {
      // if we decoded all frame, we know for sure that the total duration
      // is the last PTS value
      if (duration.has_value())
        assert(duration.value() == last_pts);
      duration = last_pts;
      return {std::move(frame), false};
    }

    auto [data, pts] = decoder.get_next_frame();
    if (!frame)
      frame = tp::ffmpeg::Frame::create();

    frame->width = decoder.width();
    frame->height = decoder.height();
    frame->duration = pts - last_pts;
    frame->pts = std::exchange(last_pts, pts);
    frame->format = tp::ffmpeg::PixelFormat::AV_PIX_FMT_RGBA;

    frame.get_buffer();
    std::copy(data.begin(), data.end(), frame->data[0]);
    return {std::move(frame), true};
  }

  bool seek(i64 pos) override {
    decoder.reset();
    last_pts = 0;
    return true;
  }

  std::optional<i32> get_num_frames() override { return decoder.num_frames(); }
  std::optional<i64> get_duration() override { return duration; }

private:
  std::vector<char> data;
  tp::webp::AnimDecoder decoder;
  i64 last_pts = 0;
  std::optional<i64> duration;
};
#endif

} // namespace vkvideo::medias
