#pragma once

#include "vkvideo/core/types.hpp"
#include "vkvideo/graphics/tlsem.hpp"
#include "vkvideo/graphics/video_frame.hpp"
#include "vkvideo/graphics/vma.hpp"
#include "vkvideo/medias/stream.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <optional>

namespace vkvideo {
class Context;

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

  virtual std::optional<i32> get_num_frames() { return std::nullopt; }
  virtual std::optional<i64> get_duration() { return std::nullopt; }

  // for debugging only
  virtual void wait_for_load(i64 timeout) {}

private:
  i64 last_time;
};

class VideoStream : public Video {
public:
  VideoStream(std::unique_ptr<Stream> stream)
      : stream{std::move(stream)}, frame{ffmpeg::Frame::create()} {}
  ~VideoStream() = default;

  std::optional<VideoFrame> get_frame_monotonic(i64 time) override;

  void seek(i64 time) override {
    Video::seek(time);
    stream->seek(time);
  }

  std::optional<i32> get_num_frames() override {
    return stream->get_num_frames();
  }

  std::optional<i64> get_duration() override { return stream->get_duration(); }

private:
  std::unique_ptr<Stream> stream;
  ffmpeg::Frame frame;
};

class VideoVRAM : public Video {
public:
  VideoVRAM(Stream &stream, Context &ctx);
  ~VideoVRAM() = default;

  std::optional<VideoFrame> get_frame_monotonic(i64 time) override;
  void seek(i64 time) override;
  void wait_for_load(i64 timeout) override;

  std::optional<i32> get_num_frames() override { return timestamps.size(); }

  std::optional<i64> get_duration() override {
    if (timestamps.empty())
      return std::nullopt;
    return timestamps.back();
  }

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
