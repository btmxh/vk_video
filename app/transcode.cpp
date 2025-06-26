extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

import std;
import vulkan_hpp;
import vk_mem_alloc_hpp;
import vkvideo;
import vkfw;

#include <cassert>

using namespace vkvideo;
using namespace vkvideo::medias;
using namespace vkvideo::graphics;
using namespace vkvideo::tp;

constexpr vk::Format RENDER_TARGET_FORMAT = vk::Format::eR32G32B32A32Sfloat;
constexpr vk::Extent2D RENDER_TARGET_EXTENT{1920, 1080};
constexpr i64 FPS = 60, DURATION = 10e9, NUM_FRAMES = DURATION / 1e9 * FPS;
constexpr ffmpeg::PixelFormat SW_PIX_FMT = ffmpeg::PixelFormat::AV_PIX_FMT_NV12;

ffmpeg::BufferRef init_codec_ctx(ffmpeg::CodecContext &cc,
                                 AVBufferRef *hw_device_ctx) {
  cc->width = RENDER_TARGET_EXTENT.width;
  cc->height = RENDER_TARGET_EXTENT.height;
  cc->time_base = {1, FPS};
  cc->framerate = {FPS, 1};
  cc->sample_aspect_ratio = {1, 1};
  cc->pix_fmt = ffmpeg::PixelFormat::AV_PIX_FMT_VULKAN;
  cc->sw_pix_fmt = SW_PIX_FMT;
  // 0.1 bit per pixel
  cc->bit_rate =
      RENDER_TARGET_EXTENT.width * RENDER_TARGET_EXTENT.height * FPS * 0.1;
  ffmpeg::BufferRef hw_frames_ctx{av_hwframe_ctx_alloc(hw_device_ctx)};
  assert(cc->hw_frames_ctx = av_buffer_ref(hw_frames_ctx.get()));
  auto &frames_ctx =
      *reinterpret_cast<AVHWFramesContext *>(hw_frames_ctx->data);
  frames_ctx.format = tp::ffmpeg::PixelFormat::AV_PIX_FMT_VULKAN;
  frames_ctx.sw_format = cc->sw_pix_fmt;
  frames_ctx.width = cc->width;
  frames_ctx.height = cc->height;
  auto vk_frames_ctx = static_cast<AVVulkanFramesContext *>(frames_ctx.hwctx);
  vk_frames_ctx->usage = static_cast<decltype(vk_frames_ctx->usage)>(
      static_cast<VkImageUsageFlags>(
          vk::ImageUsageFlagBits::eStorage |
          vk::ImageUsageFlagBits::eTransferSrc |
          vk::ImageUsageFlagBits::eVideoEncodeSrcKHR));
  if (auto [w, h, d] =
          vk::blockExtent(static_cast<vk::Format>(vk_frames_ctx->format[0]));
      w * h * d > 1) {
    vk_frames_ctx->img_flags |= static_cast<decltype(vk_frames_ctx->img_flags)>(
        vk::ImageCreateFlagBits::eBlockTexelViewCompatible);
  }
  ffmpeg::av_call(av_hwframe_ctx_init(cc->hw_frames_ctx));
  return hw_frames_ctx;
}

int main(int argc, char *argv[]) {
  namespace vkr = vk::raii;

  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <input.mkv> <output.mkv>"
              << std::endl;
    return 1;
  }

  ffmpeg::Instance ffmpeg;
  VkContext vk{true};

  auto video = medias::open_video(vk, argv[1]);

  OutputContext output_ctx{argv[2]};
  auto &&[stream, codec_ctx] =
      output_ctx.add_stream(ffmpeg::find_enc_codec("h264_vulkan"));
  auto hw_frames_ctx = init_codec_ctx(codec_ctx, vk.get_hwaccel_ctx().get());
  output_ctx.init(stream.index);

  vkr::CommandPool pool{
      vk.get_device(),
      vk::CommandPoolCreateInfo{
          .flags = vk::CommandPoolCreateFlagBits::eTransient |
                   vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
          .queueFamilyIndex =
              static_cast<u32>(vk.get_queues().get_qf_graphics()),
      }};

  auto [render_target, render_target_allocation] =
      vk.get_vma_allocator().createImageUnique(
          vk::ImageCreateInfo{
              .imageType = vk::ImageType::e2D,
              .format = RENDER_TARGET_FORMAT,
              .extent = {RENDER_TARGET_EXTENT.width,
                         RENDER_TARGET_EXTENT.height, 1},
              .mipLevels = 1,
              .arrayLayers = 1,
              .samples = vk::SampleCountFlagBits::e1,
              .tiling = vk::ImageTiling::eOptimal,
              .usage = vk::ImageUsageFlagBits::eColorAttachment |
                       vk::ImageUsageFlagBits::eStorage,
              .sharingMode = vk::SharingMode::eExclusive,
              .initialLayout = vk::ImageLayout::eUndefined,
          },
          vma::AllocationCreateInfo{
              .requiredFlags = vk::MemoryPropertyFlagBits::eDeviceLocal,
          });
  vk.set_debug_label(*render_target, "render_target");
  vk::raii::ImageView render_target_views{
      vk.get_device(),
      vk::ImageViewCreateInfo{
          .image = *render_target,
          .viewType = vk::ImageViewType::e2D,
          .format = RENDER_TARGET_FORMAT,
          .components =
              {
                  vk::ComponentSwizzle::eIdentity,
                  vk::ComponentSwizzle::eIdentity,
                  vk::ComponentSwizzle::eIdentity,
                  vk::ComponentSwizzle::eIdentity,
              },
          .subresourceRange =
              {
                  .aspectMask = vk::ImageAspectFlagBits::eColor,
                  .levelCount = 1,
                  .layerCount = 1,
              },
      }};
  vkr::CommandBuffers render_cmds{vk.get_device(),
                                  vk::CommandBufferAllocateInfo{
                                      .commandPool = *pool,
                                      .level = vk::CommandBufferLevel::ePrimary,
                                      .commandBufferCount = static_cast<u32>(1),
                                  }};
  auto render_cmd = std::move(render_cmds[0]);
  TimelineSemaphore render_sem{vk.get_device(), 0, "render_sem"};

  VideoPipelineCache pipelines;
  std::unique_ptr<HwVideoRescaler> video_rescaler = nullptr;

  output_ctx.begin();

  for (i32 i = 0; i < NUM_FRAMES; ++i) {
    vk.get_temp_pools().garbage_collect();

    auto out_frame = ffmpeg::Frame::create();
    ffmpeg::av_call(
        av_hwframe_get_buffer(hw_frames_ctx.get(), out_frame.get(), 0));
    out_frame->pts = i;
    auto &frame_data = *reinterpret_cast<AVVkFrame *>(out_frame->data[0]);
    get_cached_hw_rescaler(video_rescaler, vk.get_device(), SW_PIX_FMT);
    std::vector<vk::Image> images;
    for (auto img : frame_data.img)
      if (img)
        images.push_back(static_cast<vk::Image>(img));
    auto rescale_deps =
        video_rescaler->bind_images(vk.get_device(), *render_target, images);
    FFmpegVideoFrameData output_frame{std::move(out_frame)};

    {
      auto locked_output_frame = output_frame.lock();

      auto video_frame = video->get_frame(i * 1e9 / FPS);
      auto locked_video_frame_data =
          video_frame.transform([](auto &frame) { return frame.data->lock(); });
      auto planes =
          locked_video_frame_data
              .transform([](auto &data) { return data->get_planes(); })
              .value_or(std::vector<VideoFramePlane *>{});
      auto pipeline = pipelines.get(
          VideoPipelineInfo{
              .plane_formats =
                  planes | std::ranges::views::transform([](const auto &plane) {
                    return plane->get_format();
                  }) |
                  std::ranges::to<std::vector>(),
              .color_attachment_format = RENDER_TARGET_FORMAT,
              .pixel_format = video_frame.has_value()
                                  ? video_frame->frame_format
                                  : AV_PIX_FMT_NONE,
          },
          vk.get_device(), 1);
      auto views =
          planes | std::ranges::views::transform([&](const auto &plane) {
            return pipeline->create_image_view(vk.get_device(), *plane);
          }) |
          std::ranges::to<std::vector>();

      if (video_frame.has_value()) {
        (*locked_video_frame_data)
            ->layout_transition(video_frame->frame_index,
                                vk.get_queues().get_qf_graphics(),
                                vk.get_temp_pools(),
                                vk::PipelineStageFlagBits2::eFragmentShader,
                                vk::AccessFlagBits2::eShaderSampledRead,
                                vk::ImageLayout::eShaderReadOnlyOptimal);

        vk::DescriptorImageInfo desc_sampler{
            .sampler = pipeline->sampler,
            .imageView = views.front(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };

        // update desc set
        vk.get_device().updateDescriptorSets(
            vk::WriteDescriptorSet{
                .dstSet = *pipeline->descriptor_sets.front(),
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo = &desc_sampler,
            },
            {});
      }

      // record cmdbuf
      render_cmd.begin(vk::CommandBufferBeginInfo{
          .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

      // transition: eUndefined -> eTransferDstOptimal
      {
        vk::ImageMemoryBarrier2 sc_img_trans{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eColorAttachmentRead,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = *render_target,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1,
            }};
        render_cmd.pipelineBarrier2(
            vk::DependencyInfo{}.setImageMemoryBarriers(sc_img_trans));
      }

      // here we use the huge ass graphics pipeline
      vk::RenderingAttachmentInfo color_attachment{
          .imageView = render_target_views,
          .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
          .loadOp = vk::AttachmentLoadOp::eClear,
          .storeOp = vk::AttachmentStoreOp::eStore,
          .clearValue = {vk::ClearColorValue{
              .float32 = std::array<float, 4>{0.0f, 0.0f, 0.2f, 1.0f},
          }},
      };
      render_cmd.beginRendering(vk::RenderingInfo{
          .renderArea = {{0, 0}, RENDER_TARGET_EXTENT},
          .layerCount = 1,
      }
                                    .setColorAttachments(color_attachment));
      if (locked_video_frame_data.has_value()) {
        auto &data = **locked_video_frame_data;
        render_cmd.setViewport(
            0, vk::Viewport{
                   .x = 0,
                   .y = 0,
                   .width = static_cast<float>(RENDER_TARGET_EXTENT.width),
                   .height = static_cast<float>(RENDER_TARGET_EXTENT.height),
               });
        render_cmd.setScissor(0, vk::Rect2D{
                                     .offset = {0, 0},
                                     .extent = RENDER_TARGET_EXTENT,
                                 });
        render_cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                pipeline->pipeline);
        render_cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipeline->pipeline_layout, 0,
                                      *pipeline->descriptor_sets.front(), {});
        auto [width, height] = data.get_extent();
        auto [padded_width, padded_height] = data.get_padded_extent();
        render_cmd.pushConstants<FrameInfoPushConstants>(
            pipeline->pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
            FrameInfoPushConstants{
                .uv_max =
                    {
                        static_cast<float>(width) /
                            static_cast<float>(padded_width),
                        static_cast<float>(height) /
                            static_cast<float>(padded_height),
                    },
                .frame_index =
                    static_cast<float>(video_frame->frame_index.value_or(0.0f)),
            });
        render_cmd.draw(3, 1, 0, 0);
      }

      render_cmd.endRendering();

      {
        std::vector<vk::ImageMemoryBarrier2> barriers;

        barriers.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eColorAttachmentRead,
            .dstStageMask = video_rescaler->pipeline_stage_flags(),
            .dstAccessMask = video_rescaler->input_access_flags(),
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = *render_target,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1,
            }});
        for (auto &plane : locked_output_frame->get_planes()) {
          barriers.push_back(vk::ImageMemoryBarrier2{
              .srcStageMask = plane->get_stage_flag(),
              .srcAccessMask = plane->get_access_flag(),
              .dstStageMask = video_rescaler->pipeline_stage_flags(),
              .dstAccessMask = video_rescaler->output_access_flags(),
              .oldLayout = vk::ImageLayout::eUndefined,
              .newLayout = vk::ImageLayout::eGeneral,
              // TODO: assuming no queue family transfer needed
              .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
              .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
              .image = plane->get_image(),
              .subresourceRange = {
                  .aspectMask = vk::ImageAspectFlagBits::eColor,
                  .levelCount = 1,
                  .layerCount = 1,
              }});

          plane->commit_image_barrier(barriers.back(),
                                      plane->get_semaphore_value());
        }
        render_cmd.pipelineBarrier2(
            vk::DependencyInfo{}.setImageMemoryBarriers(barriers));
      }

      if (video_frame.has_value())
        video_rescaler->rescale(render_cmd, RENDER_TARGET_EXTENT.width,
                                RENDER_TARGET_EXTENT.height);

      render_cmd.end();

      vk::CommandBufferSubmitInfo cmd_buf_info{
          .commandBuffer = render_cmd,
      };
      std::vector<vk::SemaphoreSubmitInfo> wait_sem_info;
      std::vector<vk::SemaphoreSubmitInfo> sig_sem_info{vk::SemaphoreSubmitInfo{
          .semaphore = render_sem,
          .value = static_cast<u64>(i + 1),
          .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
      }};
      for (auto &plane : planes) {
        wait_sem_info.push_back(plane->wait_sem_info());
        sig_sem_info.push_back(plane->signal_sem_info(
            vk::PipelineStageFlagBits2::eFragmentShader));
        plane->set_semaphore_value(plane->get_semaphore_value() + 1);
      }

      {
        auto [q_lock, graphics_queue] = vk.get_queues().get_graphics_queue();
        graphics_queue.submit2(vk::SubmitInfo2{}
                                   .setCommandBufferInfos(cmd_buf_info)
                                   .setWaitSemaphoreInfos(wait_sem_info)
                                   .setSignalSemaphoreInfos(sig_sem_info));
      }
    }

    // FIXME: there are still some race conditions
    render_sem.wait(i + 1, std::numeric_limits<i64>::max());
    output_ctx.write_frame(output_frame.get(), 0);
  }

  vk.get_device().waitIdle();
  output_ctx.end();
  return 0;
}
