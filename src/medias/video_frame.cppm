module;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_vulkan.h>
}
#include <execinfo.h>

#include <cassert>

export module vkvideo.medias:video_frame;

import std;
import vulkan_hpp;
import vk_mem_alloc_hpp;
import vkvideo.core;
import vkvideo.third_party;
import vkvideo.graphics;

export namespace vkvideo::medias {

struct HWVideoFormat {
  vk::Format format;
  tp::ffmpeg::PixelFormat pixfmt;
  vk::ImageAspectFlags aspect;
  i32 vk_planes, nb_images, nb_images_fallback;
  std::vector<vk::Format> fallbacks;
};

extern const HWVideoFormat *get_hw_video_format(vk::Format format);

// holy java
class LockedVideoFramePlane {
public:
  virtual ~LockedVideoFramePlane() = default;

  virtual vk::Image get_image() const = 0;
  virtual vk::Format get_format() const = 0;
  virtual vk::ImageLayout get_image_layout() const = 0;
  virtual vk::PipelineStageFlags2 get_stage_flag() const = 0;
  virtual vk::AccessFlags2 get_access_flag() const = 0;
  virtual vk::Semaphore get_semaphore() const = 0;
  virtual u64 get_semaphore_value() const = 0;
  virtual u32 get_queue_family_idx() const = 0;
  virtual i32 get_num_layers() const = 0;

  virtual void set_image_layout(vk::ImageLayout layout) = 0;
  virtual void set_stage_flag(vk::PipelineStageFlags2 stage_flag) = 0;
  virtual void set_access_flag(vk::AccessFlags2 access_flag) = 0;
  virtual void set_semaphore(vk::Semaphore semaphore) = 0;
  virtual void set_semaphore_value(u64 semaphore_value) = 0;
  virtual void set_queue_family_idx(u32 queue_family_idx) = 0;

  vk::ImageSubresourceLayers
  get_subresource_layer(std::optional<i32> frame_idx = std::nullopt) const {
    return {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseArrayLayer = static_cast<u32>(frame_idx.value_or(0)),
        .layerCount = 1,
    };
  }

  vk::ImageSubresourceLayers get_subresource_layers() const {
    return {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .layerCount = static_cast<u32>(get_num_layers()),
    };
  }

  vk::ImageSubresourceRange get_subresource_range() const {
    return {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = static_cast<u32>(get_num_layers()),
    };
  }

  vk::ImageMemoryBarrier2
  image_barrier(vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                vk::ImageLayout img_layout,
                u32 queue_family_index = vk::QueueFamilyIgnored,
                const void *pnext = nullptr) {
    return vk::ImageMemoryBarrier2{
        .pNext = pnext,
        .srcStageMask = get_stage_flag(),
        .srcAccessMask = get_access_flag(),
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = get_image_layout(),
        .newLayout = img_layout,
        .srcQueueFamilyIndex = queue_family_index == vk::QueueFamilyIgnored
                                   ? vk::QueueFamilyIgnored
                                   : get_queue_family_idx(),
        .dstQueueFamilyIndex = get_queue_family_idx() == vk::QueueFamilyIgnored
                                   ? vk::QueueFamilyIgnored
                                   : queue_family_index,
        .image = get_image(),
        .subresourceRange = get_subresource_range(),
    };
  }

  void commit_image_barrier(const vk::ImageMemoryBarrier2 &barrier,
                            u64 new_sem_value,
                            vk::Semaphore new_sem = nullptr) {
    // maybe equal is somewhat acceptable?
    assert(get_semaphore_value() < new_sem_value);
    set_stage_flag(barrier.dstStageMask);
    set_access_flag(barrier.dstAccessMask);
    set_image_layout(barrier.newLayout);
    set_semaphore_value(new_sem_value);
    if (barrier.dstQueueFamilyIndex != vk::QueueFamilyIgnored)
      set_queue_family_idx(barrier.dstQueueFamilyIndex);
    if (new_sem)
      set_semaphore(new_sem);
  }

  vk::SemaphoreSubmitInfo wait_sem_info() {
    return vk::SemaphoreSubmitInfo{
        .semaphore = get_semaphore(),
        .value = get_semaphore_value(),
        .stageMask = get_stage_flag(),
    };
  }

  // automatic value numbering API
  // don't use this if you want to use your own sem_values
  void commit_image_barrier(const vk::ImageMemoryBarrier2 &barrier) {
    commit_image_barrier(barrier, get_semaphore_value() + 1);
  }

  vk::SemaphoreSubmitInfo signal_sem_info() {
    return vk::SemaphoreSubmitInfo{
        .semaphore = get_semaphore(),
        .value = get_semaphore_value() + 1,
        .stageMask = get_stage_flag(),
    };
  }
};

class VideoFramePlane {
public:
  virtual ~VideoFramePlane() = default;

  virtual std::unique_ptr<LockedVideoFramePlane> lock() = 0;
};

class VideoFrameData {
public:
  VideoFrameData(i32 width, i32 height,
                 std::vector<std::unique_ptr<VideoFramePlane>> planes = {},
                 UniqueAny backing_data = {}, i32 padded_width = -1,
                 i32 padded_height = -1)
      : width{width}, height{height},
        padded_width{padded_width < 0 ? width : padded_width},
        padded_height{padded_height < 0 ? height : padded_height},
        backing_data{std::move(backing_data)}, planes{std::move(planes)} {}

  std::vector<VideoFramePlane *> get_planes() {
    std::vector<VideoFramePlane *> out_planes;
    out_planes.reserve(planes.size());
    for (const auto &plane : planes)
      out_planes.push_back(plane.get());
    return out_planes;
  }

  std::pair<i32, i32> get_extent() const { return {width, height}; }

  std::pair<i32, i32> get_padded_extent() const {
    return {padded_width, padded_height};
  }

private:
  i32 width, height;
  i32 padded_width, padded_height;
  UniqueAny backing_data;
  std::vector<std::unique_ptr<VideoFramePlane>> planes;
};

class FFmpegBackedLockedVideoFramePlane : public LockedVideoFramePlane {
public:
  ~FFmpegBackedLockedVideoFramePlane() override {
    vk_frames_ctx->unlock_frame(hw_frames_ctx, frame);
  }

  AVHWFramesContext *hw_frames_ctx;
  AVVulkanFramesContext *vk_frames_ctx;
  AVVkFrame *frame;
  i32 plane_index;

  vk::Image get_image() const override { return frame->img[plane_index]; }
  vk::Format get_format() const override {
    return static_cast<vk::Format>(vk_frames_ctx->format[plane_index]);
  }
  vk::ImageLayout get_image_layout() const override {
    return static_cast<vk::ImageLayout>(frame->layout[plane_index]);
  }
  vk::PipelineStageFlags2 get_stage_flag() const override {
    return vk::PipelineStageFlagBits2::eBottomOfPipe;
  }
  vk::AccessFlags2 get_access_flag() const override {
    return static_cast<vk::AccessFlags2>(frame->access[plane_index]);
  }
  vk::Semaphore get_semaphore() const override {
    return frame->sem[plane_index];
  }
  u64 get_semaphore_value() const override {
    return frame->sem_value[plane_index];
  }
  u32 get_queue_family_idx() const override {
    return frame->queue_family[plane_index];
  }
  i32 get_num_layers() const override { return vk_frames_ctx->nb_layers; }

  void set_image_layout(vk::ImageLayout value) override {
    frame->layout[plane_index] =
        static_cast<std::decay_t<decltype(frame->layout[plane_index])>>(value);
  }
  void set_stage_flag(vk::PipelineStageFlags2 value) override {
    // (hardcoded getter returns BottomOfPipe, so you probably don't need to
    // store this — no-op) if needed, you could store somewhere
  }

  void set_access_flag(vk::AccessFlags2 value) override {
    frame->access[plane_index] =
        static_cast<std::decay_t<decltype(frame->access[plane_index])>>(
            static_cast<u64>(value));
  }
  void set_semaphore(vk::Semaphore value) override {
    frame->sem[plane_index] = value;
  }
  void set_semaphore_value(u64 value) override {
    frame->sem_value[plane_index] = value;
  }
  void set_queue_family_idx(u32 value) override {
    frame->queue_family[plane_index] = value;
  }
};

class FFmpegBackedVideoFramePlane : public VideoFramePlane {
public:
  ~FFmpegBackedVideoFramePlane() = default;

  std::unique_ptr<LockedVideoFramePlane> lock() override {
    auto ptr = std::make_unique<FFmpegBackedLockedVideoFramePlane>();
    ptr->hw_frames_ctx = hw_frames_ctx;
    ptr->vk_frames_ctx = vk_frames_ctx;
    ptr->frame = frame;
    ptr->plane_index = plane_index;

    vk_frames_ctx->lock_frame(hw_frames_ctx, frame);
    return ptr;
  }

  AVHWFramesContext *hw_frames_ctx;
  AVVulkanFramesContext *vk_frames_ctx;
  AVVkFrame *frame;
  i32 plane_index;
};

class StructLockedVideoFramePlane : public LockedVideoFramePlane {
public:
  ~StructLockedVideoFramePlane() override = default;

  vk::Image image;
  vk::Format format;
  vk::ImageLayout layout;
  vk::PipelineStageFlags2 stage;
  vk::AccessFlags2 access;
  vk::Semaphore semaphore;
  u64 semaphore_value;
  u32 queue_family_idx;
  i32 num_layers;

  vk::Image get_image() const override { return image; }
  vk::Format get_format() const override { return format; }
  vk::ImageLayout get_image_layout() const override { return layout; }
  vk::PipelineStageFlags2 get_stage_flag() const override { return stage; }
  vk::AccessFlags2 get_access_flag() const override { return access; }
  vk::Semaphore get_semaphore() const override { return semaphore; }
  u64 get_semaphore_value() const override { return semaphore_value; }
  u32 get_queue_family_idx() const override { return queue_family_idx; }
  i32 get_num_layers() const override { return num_layers; }

  void set_image_layout(vk::ImageLayout value) override { layout = value; }
  void set_stage_flag(vk::PipelineStageFlags2 value) override { stage = value; }
  void set_access_flag(vk::AccessFlags2 value) override { access = value; }
  void set_semaphore(vk::Semaphore value) override { semaphore = value; }
  void set_semaphore_value(u64 value) override { semaphore_value = value; }
  void set_queue_family_idx(u32 value) override { queue_family_idx = value; }
};

class StructVideoFramePlane : public VideoFramePlane {
public:
  ~StructVideoFramePlane() = default;

  std::unique_ptr<LockedVideoFramePlane> lock() override {
    return std::make_unique<StructLockedVideoFramePlane>(locked);
  }

  StructLockedVideoFramePlane locked;
};

struct VideoFrame {
  // static constexpr std::size_t max_num_images = AV_NUM_DATA_POINTERS;
  std::shared_ptr<VideoFrameData> data;
  tp::ffmpeg::PixelFormat frame_format;
  std::optional<i32> frame_index; // available for texture arrays
};

VideoFrame upload_frames_to_gpu(graphics::VkContext &vk,
                                std::span<tp::ffmpeg::Frame> frames) {
  assert(!frames.empty());

  i32 width = frames.front()->width;
  i32 height = frames.front()->height;
  bool has_alpha =
      tp::ffmpeg::get_pix_fmt_desc(
          static_cast<tp::ffmpeg::PixelFormat>(frames.front()->format))
          ->flags &
      static_cast<int>(tp::ffmpeg::PixelFormatFlagBits::eHasAlpha);

  // for convenience, we will only use RGB formats,
  // as YUV blitting is explicitly disallowed in Vulkan spec

  std::map<tp::ffmpeg::PixelFormat, std::vector<vk::Format>> supported_formats{
      {AV_PIX_FMT_GRAY8, {vk::Format::eR8Unorm}},
      {AV_PIX_FMT_GRAY8A, {vk::Format::eR8G8Unorm}},
      {AV_PIX_FMT_RGB24, {vk::Format::eR8G8B8Unorm}},
      {AV_PIX_FMT_RGBA, {vk::Format::eR8G8B8A8Unorm}},
  };
  for (auto &[pix_fmt, vk_formats] : supported_formats) {
    std::erase_if(vk_formats, [&](vk::Format format) {
      try {
        auto &physical_device = vk.get_physical_device();
        auto fmt = physical_device.getFormatProperties(format);
        auto flag = vk::FormatFeatureFlagBits::eTransferDst |
                    vk::FormatFeatureFlagBits::eTransferSrc |
                    vk::FormatFeatureFlagBits::eSampledImage;
        if ((fmt.optimalTilingFeatures & flag) != flag) {
          return true;
        }
        auto props = physical_device.getImageFormatProperties(
            format, vk::ImageType::e2D, vk::ImageTiling::eOptimal,
            vk::ImageUsageFlagBits::eTransferDst |
                vk::ImageUsageFlagBits::eTransferSrc |
                vk::ImageUsageFlagBits::eSampled);
        return props.maxExtent.width < width ||
               props.maxExtent.height < height ||
               props.maxArrayLayers < frames.size();
      } catch (vk::FormatNotSupportedError &ex) {
        return true;
      }
    });
  };

  std::vector<AVPixelFormat> formats;
  for (auto &[pix_fmt, vk_formats] : supported_formats) {
    if (!vk_formats.empty()) {
      formats.push_back(pix_fmt);
    }
  }
  formats.push_back(AV_PIX_FMT_NONE);

  auto format = avcodec_find_best_pix_fmt_of_list(
      formats.data(), static_cast<AVPixelFormat>(frames.front()->format),
      has_alpha, nullptr);
  if (format == AV_PIX_FMT_NONE) {
    throw std::runtime_error{"No supported format"};
  }

  std::vector<tp::ffmpeg::Frame> rescaled_frames;
  tp::ffmpeg::VideoRescaler rescaler{};
  for (const auto &frame : frames) {
    auto rescaled_frame = tp::ffmpeg::Frame::create();
    rescaled_frame->width = width;
    rescaled_frame->height = height;
    rescaled_frame->format = format;

    rescaler.auto_rescale(rescaled_frame, frame);
    rescaled_frames.push_back(std::move(rescaled_frame));
  }

  auto linesize =
      static_cast<std::size_t>(rescaled_frames.front()->linesize[0]);
  auto num_comps = tp::ffmpeg::get_pix_fmt_desc(format)->nb_components;

  // creating vulkan image...
  auto &device = vk.get_device();

  auto [uniq_image, uniq_image_allocation] =
      vk.get_vma_allocator().createImageUnique(
          {
              .imageType = vk::ImageType::e2D,
              .format = supported_formats[format].front(),
              .extent = vk::Extent3D{static_cast<u32>(width),
                                     static_cast<u32>(height), 1},
              .mipLevels = 1,
              .arrayLayers = static_cast<u32>(rescaled_frames.size()),
              .samples = vk::SampleCountFlagBits::e1,
              .tiling = vk::ImageTiling::eOptimal,
              .usage = vk::ImageUsageFlagBits::eSampled |
                       vk::ImageUsageFlagBits::eTransferSrc |
                       vk::ImageUsageFlagBits::eTransferDst,
              .sharingMode = vk::SharingMode::eExclusive,
              .initialLayout = vk::ImageLayout::eUndefined,
          },
          {
              .requiredFlags = vk::MemoryPropertyFlagBits::eDeviceLocal,
          });
  auto image = vk::raii::Image{vk.get_device(), uniq_image.release()};
  auto image_allocation = std::move(uniq_image_allocation);
  i32 frame_size = linesize * height;
  auto [buffer, buffer_alloc] = vk.get_vma_allocator().createBufferUnique(
      {
          .size = frame_size * rescaled_frames.size(),
          .usage = vk::BufferUsageFlagBits::eTransferSrc,
      },
      {
          .flags = vma::AllocationCreateFlagBits::eMapped,
          .requiredFlags = vk::MemoryPropertyFlagBits::eHostVisible,
      });

  u8 *data = static_cast<u8 *>(
      vk.get_vma_allocator().getAllocationInfo(buffer_alloc.get()).pMappedData);
  for (const auto &frame : rescaled_frames) {
    std::memcpy(data, frame->data[0], frame_size);
    data += frame_size;
  }

  auto &tx_pool = vk.get_temp_pools();
  auto cmd_buf = tx_pool.begin(vk.get_queues().get_qf_transfer());
  cmd_buf.begin(vk::CommandBufferBeginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
  vk::ImageMemoryBarrier2 img_barrier{
      .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
      .srcAccessMask = vk::AccessFlagBits2::eNone,
      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
      .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
      .oldLayout = vk::ImageLayout::eUndefined,
      .newLayout = vk::ImageLayout::eTransferDstOptimal,
      .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
      .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
      .image = *image,
      .subresourceRange = {
          .aspectMask = vk::ImageAspectFlagBits::eColor,
          .levelCount = 1,
          .layerCount = static_cast<u32>(rescaled_frames.size()),
      }};
  cmd_buf.pipelineBarrier2(
      vk::DependencyInfo{}.setImageMemoryBarriers(img_barrier));
  cmd_buf.copyBufferToImage(
      *buffer, *image, vk::ImageLayout::eTransferDstOptimal,
      vk::BufferImageCopy{
          .bufferRowLength = static_cast<u32>(linesize / num_comps),
          .imageSubresource =
              vk::ImageSubresourceLayers{
                  .aspectMask = vk::ImageAspectFlagBits::eColor,
                  .layerCount = static_cast<u32>(rescaled_frames.size()),
              },
          .imageExtent = vk::Extent3D{static_cast<u32>(width),
                                      static_cast<u32>(height), 1},
      });
  cmd_buf.end();
  auto [sem, sem_value] = tx_pool.end(
      std::move(cmd_buf), vk.get_queues().get_qf_transfer(), std::move(buffer));

  auto plane = std::make_unique<StructVideoFramePlane>();
  plane->locked.image = *image;
  plane->locked.format = supported_formats[format].front();
  plane->locked.layout = vk::ImageLayout::eTransferDstOptimal;
  plane->locked.stage = vk::PipelineStageFlagBits2::eTransfer;
  plane->locked.access = vk::AccessFlagBits2::eTransferWrite;
  plane->locked.semaphore = *sem;
  plane->locked.semaphore_value = sem_value;
  plane->locked.queue_family_idx = vk.get_queues().get_qf_transfer();
  plane->locked.num_layers = static_cast<i32>(rescaled_frames.size());

  std::vector<std::unique_ptr<VideoFramePlane>> planes;
  planes.emplace_back(std::move(plane));

  return VideoFrame{
      std::make_shared<VideoFrameData>(
          width, height, std::move(planes),
          std::make_tuple(std::move(image), std::move(image_allocation),
                          std::move(sem))),
      format};
}

} // namespace vkvideo::medias
