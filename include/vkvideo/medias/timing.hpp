#pragma once

#include "vkvideo/core/types.hpp"
#include "vkvideo/medias/media.hpp"
namespace vkvideo {

class TimeRemapper {
public:
  virtual ~TimeRemapper() = default;
  virtual i64 remap(i64 time) = 0;
};

class MediaRepeatTimeRemapper {
public:
  MediaRepeatTimeRemapper(i64 total_duration) : total_duration{total_duration} {
    assert(total_duration > 0 && "total_duration must be > 0");
  }

  template <class Frame> MediaRepeatTimeRemapper(Media<Frame> &media) {}

  virtual ~MediaRepeatTimeRemapper() = default;

  i64 remap(i64 time) { return time % total_duration; }

private:
  i64 total_duration;
};

} // namespace vkvideo
