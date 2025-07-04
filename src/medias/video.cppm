module;

#include <cassert>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_vulkan.h>
}

export module vkvideo.medias:video;

import std;
import vk_mem_alloc_hpp;
import vulkan_hpp;
import vkvideo.core;
import vkvideo.third_party;
import vkvideo.graphics;
import :video_frame;
import :stream;

namespace vkvideo::medias {
inline bool is_webp_path(std::string_view path) {
  std::ifstream file(path.data(), std::ios::binary);
  if (!file) {
    return false;
  }

  std::array<unsigned char, 12> header{};
  if (!file.read(reinterpret_cast<char *>(header.data()), header.size())) {
    return false;
  }

  // Check for RIFF + 4 unknown bytes + WEBPVP8
  return header[0] == 0x52 && header[1] == 0x49 && header[2] == 0x46 &&
         header[3] == 0x46 && header[8] == 0x57 && header[9] == 0x45 &&
         header[10] == 0x42 && header[11] == 0x50;
}

} // namespace vkvideo::medias

export namespace vkvideo::medias {

class Video {
public:
  Video() = default;
  virtual ~Video() = default;

  std::optional<VideoFrame> get_frame(i64 time) {
    if (time < last_time)
      seek(time);
    return get_frame_monotonic(time);
  }

  virtual std::optional<VideoFrame> get_frame_monotonic(i64 time) {
    assert(time >= last_time && "Monotonic constraint violation. For random "
                                "access use get_frame instead");
    last_time = time;
    return std::nullopt;
  }
  virtual void seek(i64 time) { last_time = time; }

  // must be exact, if not return nullopt
  virtual std::optional<i32> get_num_frames() { return std::nullopt; }
  virtual std::optional<i64> get_duration() { return std::nullopt; }

  // for debugging only
  virtual void wait_for_load(graphics::VkContext &vk, i64 timeout) {}

private:
  i64 last_time{0};
};

class VideoStream : public Video {
public:
  VideoStream(std::unique_ptr<Stream> stream, graphics::VkContext &vk)
      : stream{std::move(stream)}, frame{tp::ffmpeg::Frame::create()}, vk{vk} {}
  ~VideoStream() = default;

  std::optional<VideoFrame> get_frame_monotonic(i64 time) override {
    Video::get_frame_monotonic(time);
    while (true) {
      if (frame->pts + frame->duration > time) {
        if (current_video_frame.has_value())
          return current_video_frame;

        if (frame->format == AV_PIX_FMT_VULKAN) {
          auto hw_frames_ctx =
              reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
          // internally, AVFrames using ref count to manage ownership, much like
          // std::shared_ptr. These two lines basically make a new frame that
          // points to the same frame data (i.e. copying shared_ptr's)
          auto backed_frame = tp::ffmpeg::Frame::create();
          backed_frame.ref_to(frame);

          auto data =
              std::make_shared<FFmpegVideoFrameData>(std::move(backed_frame));
          return current_video_frame.emplace(data, hw_frames_ctx->sw_format);
        } else {
          return current_video_frame.emplace(upload_frames_to_gpu(
              vk, std::span<tp::ffmpeg::Frame>{&frame, 1}));
        }
      }

      current_video_frame = std::nullopt;
      bool got_frame;
      std::tie(frame, got_frame) = stream->next_frame(std::move(frame));
      if (!got_frame) {
        return std::nullopt;
      }
    }
  }

  void seek(i64 time) override {
    Video::seek(time);
    frame.unref();
    current_video_frame.reset();
    stream->seek(time);
  }

  std::optional<i32> get_num_frames() override {
    return stream->get_num_frames();
  }

  std::optional<i64> get_duration() override { return stream->get_duration(); }

private:
  graphics::VkContext &vk;
  std::unique_ptr<Stream> stream;
  tp::ffmpeg::Frame frame;
  std::optional<VideoFrame> current_video_frame;
};

class VideoVRAM : public Video {
public:
  VideoVRAM(Stream &stream, graphics::VkContext &vk) {
    stream.seek(0);
    std::vector<tp::ffmpeg::Frame> frames;
    while (true) {
      auto [frame, got_frame] = stream.next_frame();
      if (!got_frame)
        break;
      timestamps.push_back(frame->pts + frame->duration);
      frames.push_back(std::move(frame));
    }

    auto gpu_frames = upload_frames_to_gpu(vk, frames);
    frame_data = std::move(gpu_frames.data);
    format = gpu_frames.frame_format;
  }

  ~VideoVRAM() = default;

  std::optional<VideoFrame> get_frame_monotonic(i64 time) override {
    Video::get_frame_monotonic(time);
    if (timestamps.empty() || time < 0 || time > timestamps.back())
      return std::nullopt;

    assert(last_frame_idx >= 0 && last_frame_idx <= timestamps.size());

    while (time >= timestamps[last_frame_idx] &&
           last_frame_idx < timestamps.size())
      last_frame_idx++;

    if (last_frame_idx == timestamps.size())
      return std::nullopt;

    // output last_frame_idx-th frame
    return VideoFrame{
        .data = frame_data,
        .frame_format = format,
        .frame_index = last_frame_idx,
    };
  }

  void seek(i64 time) override {
    Video::seek(time);
    last_frame_idx =
        std::lower_bound(timestamps.begin(), timestamps.end(), time) -
        timestamps.begin();
  }

  std::optional<i32> get_num_frames() override { return timestamps.size(); }

  std::optional<i64> get_duration() override {
    if (timestamps.empty())
      return std::nullopt;
    return timestamps.back();
  }

private:
  static constexpr u64 sem_value = 1;
  std::shared_ptr<VideoFrameData> frame_data;

  // i-th frame is shown from [timestamps[i-1], timestamps[i])
  // (wlog assuming timestamps[-1] = 0)
  std::vector<i64> timestamps;
  i32 last_frame_idx = 0;
  tp::ffmpeg::PixelFormat format;
};

enum class DecoderType {
  eAuto = 0, // libwebp if is webp file, ffmpeg otherwise
  eFFmpeg,
  eLibWebP,
};

enum class DecodeMode {
  eAuto = 0,
  eStream,  // stream the file (for large media files)
  eReadAll, // read everything to RAM/VRAM (for small media clips)
};

struct VideoArgs {
  DecoderType type = DecoderType::eAuto;
  medias::HWAccel hwaccel = medias::HWAccel::eAuto;
  DecodeMode mode = DecodeMode::eAuto;
};

std::unique_ptr<Video> open_video(graphics::VkContext &vk,
                                  std::string_view path,
                                  const VideoArgs &args = {}) {
  DecoderType type = args.type;
  if (type == DecoderType::eAuto) {
    type = is_webp_path(path) ? DecoderType::eLibWebP : DecoderType::eFFmpeg;
  }

  DecodeMode mode = args.mode;
  // 16 MiB
  constexpr static std::size_t READ_ALL_THRESHOLD = std::size_t{16} << 20;

  std::unique_ptr<medias::Stream> stream;
  // TODO: respect the VKVIDEO_HAVE_WEBP flag
  switch (type) {
  case DecoderType::eFFmpeg: {
    medias::RawFFmpegStream raw_ffmpeg_stream{path,
                                              tp::ffmpeg::MediaType::Video};
    if (mode == DecodeMode::eAuto) {
      mode = raw_ffmpeg_stream.est_vram_bytes().value_or(
                 std::numeric_limits<std::size_t>::max()) <= READ_ALL_THRESHOLD
                 ? DecodeMode::eReadAll
                 : DecodeMode::eStream;
    }

    auto hwaccel = args.hwaccel;
    if (mode == DecodeMode::eReadAll) {
      if (hwaccel == medias::HWAccel::eOn)
        std::println(
            std::cerr,
            "Hardware-acceleration is not supported for read-all decode mode");
      hwaccel = medias::HWAccel::eOff;
    } else {
      if (hwaccel == medias::HWAccel::eAuto)
        hwaccel = medias::HWAccel::eOn;
    }

    stream = std::make_unique<medias::FFmpegStream>(
        std::move(raw_ffmpeg_stream), vk.get_hwaccel_ctx(), hwaccel);
    break;
  }
  case DecoderType::eLibWebP: {
    std::ifstream input{path.data(), std::ios::binary};
    input.exceptions(std::ios::failbit | std::ios::badbit);
    std::vector<char> webp_data(std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{});
    if (mode == DecodeMode::eAuto) {
      tp::webp::Demuxer demuxer{std::span<const u8>{
          reinterpret_cast<const u8 *>(webp_data.data()),
          reinterpret_cast<const u8 *>(webp_data.data() + webp_data.size())}};
      // currently we are not handling anything special with non-RGBA formats
      auto est_bytes = static_cast<std::size_t>(demuxer.num_frames()) *
                       demuxer.width() * demuxer.height() * 4;
      mode = est_bytes <= READ_ALL_THRESHOLD ? DecodeMode::eReadAll
                                             : DecodeMode::eStream;
    }

    if (args.hwaccel == medias::HWAccel::eOn) {
      std::println(
          std::cerr,
          "Hardware acceleration is not supported for libwebp decoder");
    }

    stream = std::make_unique<medias::AnimWebPStream>(std::move(webp_data));
    break;
  }
  default:
    throw std::runtime_error{"Invalid decoder type"};
  }

  switch (mode) {
  case DecodeMode::eStream:
    return std::make_unique<medias::VideoStream>(std::move(stream), vk);
  case DecodeMode::eReadAll:
    return std::make_unique<medias::VideoVRAM>(*stream, vk);
  default:;
  }

  throw std::runtime_error{"Invalid decode mode"};
}

} // namespace vkvideo::medias
