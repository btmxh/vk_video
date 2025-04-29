module;

#ifdef VKVIDEO_HAVE_WEBP
#include <webp/demux.h>
#endif

export module vkvideo.third_party:webp;

import std;
import vkvideo.core;

#ifdef VKVIDEO_HAVE_WEBP
namespace vkvideo::tp::webp::details {
struct WebPAnimDecoderDeleter {
  void operator()(WebPAnimDecoder *decoder) { WebPAnimDecoderDelete(decoder); }
};

struct WebPDemuxerDeleter {
  void operator()(WebPDemuxer *demuxer) { WebPDemuxDelete(demuxer); }
};
} // namespace vkvideo::tp::webp::details

export namespace vkvideo::tp::webp {

struct Info {
  i32 width, height, num_frames;
};

class AnimDecoder {
public:
  AnimDecoder(std::span<const u8> data)
      : data{WebPData{
            .bytes = data.data(),
            .size = data.size_bytes(),
        }},
        decoder{
            decltype(this->decoder)(WebPAnimDecoderNew(&this->data, nullptr))},
        info{[&]() {
          WebPAnimInfo info;
          if (!WebPAnimDecoderGetInfo(decoder.get(), &info)) {
            throw std::runtime_error{"Unable to get info from WebP decoder."};
          }

          return Info{static_cast<i32>(info.canvas_width),
                      static_cast<i32>(info.canvas_height),
                      static_cast<i32>(info.frame_count)};
        }()} {}

  AnimDecoder(AnimDecoder &&) = delete;
  AnimDecoder &operator=(AnimDecoder &&) = delete;

  i32 width() const { return info.width; }
  i32 height() const { return info.height; }
  i32 num_frames() const { return info.num_frames; }

  void reset() { WebPAnimDecoderReset(decoder.get()); }

  bool has_more_frames() const {
    return WebPAnimDecoderHasMoreFrames(decoder.get());
  }

  std::pair<std::span<u8>, i64> get_next_frame() {
    u8 *data;
    int timestamp;
    if (!WebPAnimDecoderGetNext(decoder.get(), &data, &timestamp)) {
      throw std::runtime_error{"WebP decoding error."};
    }

    return {std::span<u8>(data, data + info.width * info.height * 4),
            timestamp * 1000000};
  }

private:
  WebPData data;
  std::unique_ptr<WebPAnimDecoder, details::WebPAnimDecoderDeleter> decoder;
  Info info;
};

class Demuxer {
public:
  Demuxer(std::span<const u8> data)
      : data{.bytes = data.data(), .size = data.size_bytes()},
        demuxer{WebPDemux(&this->data)} {
    if (WebPDemuxGetFrame(demuxer.get(), 1, &iter))
      do {
        duration += iter.duration * 1000000;
      } while (WebPDemuxNextFrame(&iter));
  }

  u32 width() const {
    return WebPDemuxGetI(demuxer.get(),
                         WebPFormatFeature::WEBP_FF_CANVAS_WIDTH);
  }
  u32 height() const {
    return WebPDemuxGetI(demuxer.get(),
                         WebPFormatFeature::WEBP_FF_CANVAS_HEIGHT);
  }
  u32 num_frames() const {
    return WebPDemuxGetI(demuxer.get(), WebPFormatFeature::WEBP_FF_FRAME_COUNT);
  }

  i64 total_duration() { return duration; }

private:
  WebPData data;
  std::unique_ptr<WebPDemuxer, details::WebPDemuxerDeleter> demuxer;
  WebPIterator iter;
  i64 duration = 0;
};

} // namespace vkvideo::tp::webp
#endif
