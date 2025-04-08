#pragma once

#include "mcpp/unique_any.hpp"
#include "vkvideo/core/types.hpp"

#include <memory>

namespace vkvideo {
struct VideoFramePlane {
  vk::Image image;
  vk::Format format;
  vk::ImageLayout layout;
  vk::PipelineStageFlags2 stage;
  vk::AccessFlags2 access;
  vk::Semaphore semaphore;
  u64 semaphore_value;
  u32 queue_family_idx;
  i32 num_layers;
};

struct VideoFrameData {
  std::vector<VideoFramePlane> planes;
  // the resource that owns the data in `planes`
  // (the images and the semaphores)
  mcpp::unique_any buf;
  i32 width, height;
};

struct VideoFrame {
  // static constexpr std::size_t max_num_images = AV_NUM_DATA_POINTERS;
  std::shared_ptr<VideoFrameData> data;
  std::optional<i32> frame_index; // available for texture arrays
};
} // namespace vkvideo
