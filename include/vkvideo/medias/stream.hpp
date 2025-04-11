#pragma once

#include "vkvideo/core/types.hpp"
#include "vkvideo/medias/ffmpeg.hpp"

#include <libavformat/avformat.h>

#ifdef VKVIDEO_HAVE_WEBP
#include "webp/demux.h"
#endif

#include <optional>

namespace vkvideo {
class Context;
class Stream {
public:
  Stream() = default;
  virtual ~Stream() = default;

  virtual std::pair<ffmpeg::Frame, bool>
  next_frame(ffmpeg::Frame &&frame = {}) = 0;
  virtual bool seek(i64 pos) { return false; }
  virtual std::optional<i32> get_num_frames() { return std::nullopt; }
  virtual std::optional<i64> get_duration() { return std::nullopt; }
};

enum class HWAccel {
  eAuto = 0,
  eOff,
  eOn,
};

class FFmpegStream : public Stream {
public:
  FFmpegStream(std::string_view path, Context &ctx,
               ffmpeg::MediaType stream_type = ffmpeg::MediaType::Video,
               HWAccel hwaccel = HWAccel::eAuto);
  ~FFmpegStream() = default;

  std::pair<ffmpeg::Frame, bool>
  next_frame(ffmpeg::Frame &&frame = {}) override;
  bool seek(i64 pos) override;

  std::optional<i32> get_num_frames() override;
  std::optional<i64> get_duration() override;

  ffmpeg::InputFormatContext &get_demuxer() { return demuxer; }
  ffmpeg::CodecContext &get_decoder() { return decoder; }

private:
  ffmpeg::InputFormatContext demuxer;
  ffmpeg::CodecContext decoder;
  ffmpeg::Packet current_packet;

  i32 stream_idx;
  bool reach_eof_packet = false;

  bool read_packet();
};

#ifdef VKVIDEO_HAVE_WEBP
class AnimWebPStream : public Stream {
public:
  AnimWebPStream(std::string_view path);
  ~AnimWebPStream() = default;

  std::pair<ffmpeg::Frame, bool>
  next_frame(ffmpeg::Frame &&frame = {}) override;
  bool seek(i64 pos) override;

  std::optional<i32> get_num_frames() override;
  std::optional<i64> get_duration() override;

private:
  struct WebPAnimDecoderDeleter {
    void operator()(WebPAnimDecoder *decoder) {
      WebPAnimDecoderDelete(decoder);
    }
  };

  std::vector<u8> data;
  WebPData data_ptr;
  std::unique_ptr<WebPAnimDecoder, WebPAnimDecoderDeleter> decoder;
  WebPAnimInfo info;
  i64 last_pts = 0;
  std::optional<i64> duration;
};
#endif

} // namespace vkvideo
