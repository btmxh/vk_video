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
};

} // namespace vkvideo
