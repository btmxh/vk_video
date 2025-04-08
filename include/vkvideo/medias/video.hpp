#pragma once

#include "vkvideo/core/types.hpp"
#include "vkvideo/graphics/tlsem.hpp"
#include "vkvideo/graphics/video_frame.hpp"
#include "vkvideo/graphics/vma.hpp"
#include "vkvideo/medias/stream.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <memory>

namespace vkvideo {
class Context;

class Video {
public:
  Video() = default;
  virtual ~Video() = default;

  virtual std::optional<VideoFrame> get_frame(i64 time) = 0;
  virtual void seek(i64 time) {}
  virtual void wait_for_load(i64 timeout) {}
};

class VideoStream : public Video {
public:
  VideoStream(std::unique_ptr<Stream> stream)
      : stream{std::move(stream)}, frame{ffmpeg::Frame::create()} {}
  ~VideoStream() = default;

  std::optional<VideoFrame> get_frame(i64 time) override;
  void seek(i64 time) override { stream->seek(time); }

private:
  std::unique_ptr<Stream> stream;
  ffmpeg::Frame frame;
};

class VideoVRAM : public Video {
public:
  VideoVRAM(Stream &stream, Context &ctx);
  ~VideoVRAM() = default;

  std::optional<VideoFrame> get_frame(i64 time) override;
  void seek(i64 time) override;
  void wait_for_load(i64 timeout) override;

private:
  static constexpr u64 sem_value = 1;
  VmaImage image = nullptr;
  std::shared_ptr<TimelineSemaphore> sem;
  std::shared_ptr<VideoFrameData> frame_data;

  // i-th frame is shown from [timestamps[i-1], timestamps[i])
  // (wlog assuming timestamps[-1] = 0)
  std::vector<i64> timestamps;
  i32 last_frame_idx = 0;
};
} // namespace vkvideo
