#include "vkvideo/medias/video.hpp"

#include "vkvideo/context/context.hpp"
#include "vkvideo/graphics/vma.hpp"
#include "vkvideo/medias/ffmpeg.hpp"
#include "vkvideo/medias/stb_image_write.hpp"

#include <libavutil/hwcontext.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_enums.hpp>

#include <map>
#include <vector>

extern "C" {
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

namespace vkvideo {
void VideoStream::seek(i64 time) {
  Video::seek(time);
  av_frame_unref(frame.get());
  stream->seek(time);
}

std::optional<VideoFrame> VideoStream::get_frame_monotonic(i64 time) {
  Video::get_frame_monotonic(time);
  assert(frame);
  while (true) {
    if (frame->pts + frame->duration > time) {
      // TODO: add support for non-HWaccel formats
      if (frame->format == AV_PIX_FMT_VULKAN) {
        AVVkFrame &vk_frame = *reinterpret_cast<AVVkFrame *>(frame->data[0]);

        // TODO: maybe use a better way to fetch the formats?
        auto ffmpeg_st = dynamic_cast<FFmpegStream *>(stream.get());
        auto &decoder = ffmpeg_st->get_decoder();
        assert(decoder->hw_frames_ctx);
        auto hw_frames_ctx =
            reinterpret_cast<AVHWFramesContext *>(decoder->hw_frames_ctx->data);
        auto vk_frames_ctx =
            static_cast<AVVulkanFramesContext *>(hw_frames_ctx->hwctx);

        VideoFrame vid_frame;
        vid_frame.data = std::make_shared<VideoFrameData>();
        vid_frame.data->width = frame->width;
        vid_frame.data->height = frame->height;
        vid_frame.data->padded_width = hw_frames_ctx->width;
        vid_frame.data->padded_height = hw_frames_ctx->height;
        // auto buf_frame = ffmpeg::Frame::create();
        // buf_frame.ref_to(frame);
        // vid_frame.data->buf = std::move(buf_frame);
        for (i32 i = 0; i < std::size(vk_frame.img) && vk_frame.img[i]; ++i) {
          auto &plane = vid_frame.data->planes.emplace_back();
          plane.image = vk_frame.img[i];
          plane.format = static_cast<vk::Format>(vk_frames_ctx->format[i]);
          plane.layout = static_cast<vk::ImageLayout>(vk_frame.layout[i]);
          plane.stage = vk::PipelineStageFlagBits2::eBottomOfPipe;
          plane.access = static_cast<vk::AccessFlags2>(vk_frame.access[i]);
          plane.semaphore = vk_frame.sem[i];
          plane.semaphore_value = vk_frame.sem_value[i];
          plane.queue_family_idx = vk_frame.queue_family[i];
          plane.num_layers = vk_frames_ctx->nb_layers;
        }

        vid_frame.frame_format = hw_frames_ctx->sw_format;
        return vid_frame;
      } else {
        throw std::logic_error{"Not implemented."};
      }
    }

    bool got_frame;
    std::tie(frame, got_frame) = stream->next_frame(std::move(frame));
    if (!got_frame) {
      return std::nullopt;
    }
  }
}

VideoVRAM::VideoVRAM(Stream &stream, Context &ctx) {
  stream.seek(0);
  std::vector<ffmpeg::Frame> frames;
  while (true) {
    auto [frame, got_frame] = stream.next_frame();
    if (!got_frame)
      break;
    timestamps.push_back(frame->pts + frame->duration);
    frames.push_back(std::move(frame));
  }

  if (frames.empty()) {
    throw std::runtime_error{"Video stream has no frames"};
  }

  i32 width = frames.front()->width;
  i32 height = frames.front()->height;
  bool has_alpha =
      av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frames.front()->format))
          ->flags &
      AV_PIX_FMT_FLAG_ALPHA;

  // for convenience, we will only use RGB formats,
  // as YUV blitting is explicitly disallowed in Vulkan spec

  std::map<AVPixelFormat, std::vector<vk::Format>> supported_formats{
      {AV_PIX_FMT_GRAY8, {vk::Format::eR8Unorm}},
      {AV_PIX_FMT_GRAY8A, {vk::Format::eR8G8Unorm}},
      {AV_PIX_FMT_RGB24, {vk::Format::eR8G8B8Unorm}},
      {AV_PIX_FMT_RGBA, {vk::Format::eR8G8B8A8Unorm}},
  };
  for (auto &[pix_fmt, vk_formats] : supported_formats) {
    std::erase_if(vk_formats, [&](vk::Format format) {
      try {
        auto fmt =
            ctx.get_vulkan().get_physical_device().getFormatProperties(format);
        auto flag = vk::FormatFeatureFlagBits::eTransferDst |
                    vk::FormatFeatureFlagBits::eTransferSrc |
                    vk::FormatFeatureFlagBits::eSampledImage;
        if ((fmt.optimalTilingFeatures & flag) != flag) {
          return true;
        }
        auto props =
            ctx.get_vulkan().get_physical_device().getImageFormatProperties(
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

  format = avcodec_find_best_pix_fmt_of_list(
      formats.data(), static_cast<AVPixelFormat>(frames.front()->format),
      has_alpha, nullptr);
  if (format == AV_PIX_FMT_NONE) {
    throw std::runtime_error{"No supported format"};
  }

  std::vector<ffmpeg::Frame> rescaled_frames;
  ffmpeg::VideoRescaler rescaler{};
  for (const auto &frame : frames) {
    auto rescaled_frame = ffmpeg::Frame::create();
    rescaled_frame->width = width;
    rescaled_frame->height = height;
    rescaled_frame->format = format;

    rescaler.auto_rescale(rescaled_frame, frame);
    rescaled_frames.push_back(std::move(rescaled_frame));
  }

  frames.clear();

  auto linesize =
      static_cast<std::size_t>(rescaled_frames.front()->linesize[0]);
  auto num_comps = av_pix_fmt_desc_get(format)->nb_components;

  // creating vulkan image...
  auto &vk = ctx.get_vulkan();
  auto &device = vk.get_device();

  image = vk.get_vma_allocator().createImage(
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
          .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      });
  i32 frame_size = linesize * height;
  auto buffer = vk.get_vma_allocator().createBuffer(
      {
          .size = frame_size * rescaled_frames.size(),
          .usage = vk::BufferUsageFlagBits::eTransferSrc,
      },
      {
          .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
          .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
      });

  u8 *data = static_cast<u8 *>(buffer.get_alloc_info().pMappedData);
  for (const auto &frame : rescaled_frames) {
    std::memcpy(data, frame->data[0], frame_size);
    data += frame_size;
  }

  auto &tx_pool = vk.get_temp_pools();
  auto cmd_buf = tx_pool.begin(vk.get_qf_transfer());
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
      .image = image.get_image(),
      .subresourceRange = {
          .aspectMask = vk::ImageAspectFlagBits::eColor,
          .levelCount = 1,
          .layerCount = static_cast<u32>(rescaled_frames.size()),
      }};
  cmd_buf.pipelineBarrier2(
      vk::DependencyInfo{}.setImageMemoryBarriers(img_barrier));
  cmd_buf.copyBufferToImage(
      buffer.get_buffer(), image.get_image(),
      vk::ImageLayout::eTransferDstOptimal,
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
  u64 sem_value;
  std::tie(sem, sem_value) =
      tx_pool.end(std::move(cmd_buf), vk.get_qf_transfer(), std::move(buffer));

  frame_data = std::make_shared<VideoFrameData>();
  frame_data->width = width;
  frame_data->height = height;
  frame_data->padded_width = width;
  frame_data->padded_height = height;
  auto &image_plane = frame_data->planes.emplace_back();
  image_plane.image = image.get_image();
  image_plane.format = supported_formats[format].front();
  image_plane.layout = vk::ImageLayout::eTransferDstOptimal;
  image_plane.stage = vk::PipelineStageFlagBits2::eTransfer;
  image_plane.access = vk::AccessFlagBits2::eTransferWrite;
  image_plane.semaphore = *sem;
  image_plane.semaphore_value = sem_value;
  image_plane.queue_family_idx = vk.get_qf_transfer();
  image_plane.num_layers = static_cast<i32>(rescaled_frames.size());
}

std::optional<VideoFrame> VideoVRAM::get_frame_monotonic(i64 time) {
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

void VideoVRAM::seek(i64 time) {
  Video::seek(time);
  last_frame_idx =
      std::lower_bound(timestamps.begin(), timestamps.end(), time) -
      timestamps.begin();
}

void VideoVRAM::wait_for_load(i64 timeout) {
  sem->wait(frame_data->planes[0].semaphore_value, timeout);
}

} // namespace vkvideo
