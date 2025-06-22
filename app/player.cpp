import std;
import vulkan_hpp;
import vkvideo;
import vkfw;

#include <cassert>

using namespace vkvideo;
using namespace vkvideo::medias;
using namespace vkvideo::graphics;
using namespace vkvideo::context;
using namespace vkvideo::tp;

int main(int argc, char *argv[]) {
  namespace vkr = vk::raii;

  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <input.mkv>" << std::endl;
    return 1;
  }

  ffmpeg::Instance ffmpeg;

  auto vkfw = vkfw::initUnique([&]() {
    auto force_x11 = []() {
      auto env = std::getenv("VKVIDEO_FORCE_X11");
      return env && std::strcmp(env, "1") == 0;
    }();
    return vkfw::InitHints{
        .platform = force_x11 ? vkfw::Platform::eX11 : vkfw::Platform::eAny,
    };
  }());

  auto window = vkfw::createWindowUnique(640, 360, "vkvideo_player");

  VkContext vk{*window};
  auto video = medias::open_video(vk, argv[1]);

  vkr::CommandPool pool{
      vk.get_device(),
      vk::CommandPoolCreateInfo{
          .flags = vk::CommandPoolCreateFlagBits::eTransient |
                   vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
          .queueFamilyIndex =
              static_cast<u32>(vk.get_queues().get_qf_graphics()),
      }};

  auto render_target = [&]() {
    auto swapchain = std::make_unique<SwapchainRenderTargetImageProvider>(
        vk.get_physical_device(), vk.get_device(), *window, *vk.get_surface(),
        3);
    return RenderTarget{vk.get_device(), std::move(swapchain)};
  }();

  auto fif_cnt = render_target.num_fifs();
  vkr::CommandBuffers cmd_bufs{
      vk.get_device(), vk::CommandBufferAllocateInfo{
                           .commandPool = *pool,
                           .level = vk::CommandBufferLevel::ePrimary,
                           .commandBufferCount = static_cast<u32>(fif_cnt),
                       }};
  for (i32 i = 0; i < fif_cnt; ++i) {
    auto name = std::format("gfx_cmd_buf[{}]", i);
    vk.set_debug_label(*cmd_bufs[i], name.c_str());
  }
  std::vector<TimelineSemaphore> cmd_buf_end_sems;
  std::vector<std::vector<UniqueAny>> cmd_buf_dependencies;
  std::vector<u64> cmd_buf_sem_values;
  cmd_buf_end_sems.reserve(fif_cnt);
  cmd_buf_dependencies.resize(fif_cnt);
  cmd_buf_sem_values.resize(fif_cnt);
  for (i32 i = 0; i < fif_cnt; ++i) {
    auto name = std::format("gfx_cmd_buf_end_sems[{}]", i);
    cmd_buf_end_sems.emplace_back(vk.get_device(), 0, name.c_str());
  }

  // FIXME: transition layout validation error on HWAccel GPU-assisted
  // maybe related:
  // https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/10185
  auto handle_transition = [&](LockedVideoFrameData &video_frame,
                               std::optional<i32> frame_idx) {
    for (auto &plane : video_frame.get_planes()) {
      auto needs_ownership_transfer =
          plane->get_queue_family_idx() != vk.get_queues().get_qf_graphics() &&
          plane->get_queue_family_idx() != vk::QueueFamilyIgnored;
      auto needs_layout_transition =
          plane->get_image_layout() != vk::ImageLayout::eShaderReadOnlyOptimal;
      if (needs_ownership_transfer) {
        // transfer release
        {
          vk::ImageMemoryBarrier2 barrier{
              .srcStageMask = plane->get_stage_flag(),
              .srcAccessMask = plane->get_access_flag(),
              .dstStageMask = vk::PipelineStageFlagBits2::eNone,
              .dstAccessMask = vk::AccessFlagBits2::eNone,
              .oldLayout = plane->get_image_layout(),
              .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
              .srcQueueFamilyIndex = plane->get_queue_family_idx(),
              .dstQueueFamilyIndex = vk.get_queues().get_qf_graphics(),
              .image = plane->get_image(),
              .subresourceRange = plane->get_subresource_range(),
          };
          auto cmd_buf =
              vk.get_temp_pools().begin(plane->get_queue_family_idx());
          cmd_buf.begin(vk::CommandBufferBeginInfo{
              .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
          cmd_buf.pipelineBarrier2(
              vk::DependencyInfo{}.setImageMemoryBarriers(barrier));
          cmd_buf.end();
          vk.get_temp_pools().end2(
              std::move(cmd_buf), plane->get_queue_family_idx(), {},
              plane->wait_sem_info(),
              plane->signal_sem_info(vk::PipelineStageFlagBits2::eAllCommands));
        }
        plane->set_semaphore_value(plane->get_semaphore_value() + 1);
        plane->set_stage_flag(vk::PipelineStageFlagBits2::eAllCommands);
        // graphics acquire
        {
          vk::ImageMemoryBarrier2 barrier{
              .srcStageMask = vk::PipelineStageFlagBits2::eNone,
              .srcAccessMask = vk::AccessFlagBits2::eNone,
              .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
              .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
              .oldLayout = plane->get_image_layout(),
              .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
              .srcQueueFamilyIndex = plane->get_queue_family_idx(),
              .dstQueueFamilyIndex = vk.get_queues().get_qf_graphics(),
              .image = plane->get_image(),
              .subresourceRange = plane->get_subresource_range(),
          };
          auto cmd_buf =
              vk.get_temp_pools().begin(vk.get_queues().get_qf_graphics());
          cmd_buf.begin(vk::CommandBufferBeginInfo{
              .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
          cmd_buf.pipelineBarrier2(
              vk::DependencyInfo{}.setImageMemoryBarriers(barrier));
          cmd_buf.end();
          vk.get_temp_pools().end2(
              std::move(cmd_buf), vk.get_queues().get_qf_graphics(), {},
              plane->wait_sem_info(),
              plane->signal_sem_info(
                  vk::PipelineStageFlagBits2::eFragmentShader));
          plane->commit_image_barrier(barrier);
        }
      } else if (needs_layout_transition) {
        auto barrier = plane->image_barrier(
            vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eShaderSampledRead,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            static_cast<u32>(vk.get_queues().get_qf_graphics()));
        auto cmd_buf =
            vk.get_temp_pools().begin(vk.get_queues().get_qf_graphics());
        cmd_buf.begin(vk::CommandBufferBeginInfo{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        cmd_buf.pipelineBarrier2(
            vk::DependencyInfo{}.setImageMemoryBarriers(barrier));
        cmd_buf.end();
        vk.get_temp_pools().end2(std::move(cmd_buf),
                                 vk.get_queues().get_qf_graphics(), {},
                                 plane->wait_sem_info(),
                                 plane->signal_sem_info(barrier.dstStageMask));
        plane->commit_image_barrier(barrier);
      }
    }
  };

  VideoPipelineCache pipelines;
  SteadyClock clock;

  for (i32 i = 0; !window->shouldClose(); ++i) {
    vkfw::pollEvents();
    vk.get_temp_pools().garbage_collect();

    auto frame = render_target.begin_frame();
    cmd_buf_end_sems[frame.fif_idx].wait(cmd_buf_sem_values[frame.fif_idx],
                                         std::numeric_limits<i64>::max());
    // once work is done, we can free all dependencies
    cmd_buf_dependencies[frame.fif_idx].clear();
    if (auto acq_frame_opt =
            frame.acquire_image(std::numeric_limits<i64>::max());
        acq_frame_opt.has_value()) {
      auto acq_frame = acq_frame_opt.value();
      auto video_frame = video->get_frame(clock.get_time());
      auto locked_frame_data =
          video_frame.transform([](auto &frame) { return frame.data->lock(); });
      auto planes =
          locked_frame_data
              .transform([](auto &data) { return data->get_planes(); })
              .value_or(std::vector<VideoFramePlane *>{});
      auto pipeline = pipelines.get(
          VideoPipelineInfo{
              .plane_formats =
                  planes | std::ranges::views::transform([](const auto &plane) {
                    return plane->get_format();
                  }) |
                  std::ranges::to<std::vector>(),
              .color_attachment_format = acq_frame.format,
              .pixel_format = video_frame.has_value()
                                  ? video_frame->frame_format
                                  : AV_PIX_FMT_NONE,
          },
          vk.get_device(), fif_cnt);
      auto views =
          planes | std::ranges::views::transform([&](const auto &plane) {
            return pipeline->create_image_view(vk.get_device(), *plane);
          }) |
          std::ranges::to<std::vector>();

      if (video_frame.has_value()) {
        handle_transition(**locked_frame_data, video_frame->frame_index);

        vk::DescriptorImageInfo desc_sampler{
            .sampler = pipeline->sampler,
            .imageView = views.front(),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };

        // update desc set
        vk.get_device().updateDescriptorSets(
            vk::WriteDescriptorSet{
                .dstSet = *pipeline->descriptor_sets[frame.fif_idx],
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo = &desc_sampler,
            },
            {});
      }

      // record cmdbuf
      auto &cmd_buf = cmd_bufs[frame.fif_idx];
      cmd_buf.begin(vk::CommandBufferBeginInfo{
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
            .image = acq_frame.image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1,
            }};
        cmd_buf.pipelineBarrier2(
            vk::DependencyInfo{}.setImageMemoryBarriers(sc_img_trans));
      }

      // here we use the huge ass graphics pipeline
      vk::RenderingAttachmentInfo color_attachment{
          .imageView = acq_frame.view,
          .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
          .loadOp = vk::AttachmentLoadOp::eClear,
          .storeOp = vk::AttachmentStoreOp::eStore,
          .clearValue = {vk::ClearColorValue{
              .float32 = std::array<float, 4>{0.0f, 0.0f, 0.2f, 1.0f},
          }},
      };
      cmd_buf.beginRendering(vk::RenderingInfo{
          .renderArea = {{0, 0}, acq_frame.extent},
          .layerCount = 1,
      }
                                 .setColorAttachments(color_attachment));
      if (locked_frame_data.has_value()) {
        auto &data = **locked_frame_data;
        cmd_buf.setViewport(
            0, vk::Viewport{
                   .x = 0,
                   .y = 0,
                   .width = static_cast<float>(acq_frame.extent.width),
                   .height = static_cast<float>(acq_frame.extent.height),
               });
        cmd_buf.setScissor(0, vk::Rect2D{
                                  .offset = {0, 0},
                                  .extent = acq_frame.extent,
                              });
        cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                             pipeline->pipeline);
        cmd_buf.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics, pipeline->pipeline_layout, 0,
            *pipeline->descriptor_sets[frame.fif_idx], {});
        auto [width, height] = data.get_extent();
        auto [padded_width, padded_height] = data.get_padded_extent();
        cmd_buf.pushConstants<FrameInfoPushConstants>(
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
        cmd_buf.draw(3, 1, 0, 0);
      }

      cmd_buf.endRendering();

      // transition: eTransferDstOptimal -> ePresentSrcKHR
      {
        vk::ImageMemoryBarrier2 sc_img_trans{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eColorAttachmentRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstAccessMask = vk::AccessFlagBits2::eNone,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = acq_frame.image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1,
            }};
        cmd_buf.pipelineBarrier2(
            vk::DependencyInfo{}.setImageMemoryBarriers(sc_img_trans));
      }
      cmd_buf.end();

      {
        vk::CommandBufferSubmitInfo cmd_buf_info{
            .commandBuffer = cmd_buf,
        };
        std::vector<vk::SemaphoreSubmitInfo> wait_sem_info{
            {
                .semaphore = acq_frame.image_acquire_sem,
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
            },
        };
        std::vector<vk::SemaphoreSubmitInfo> sig_sem_info{
            {
                .semaphore = cmd_buf_end_sems[frame.fif_idx],
                .value = ++cmd_buf_sem_values[frame.fif_idx],
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
            },
            {
                .semaphore = acq_frame.image_present_sem,
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
            },
        };
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

        cmd_buf_dependencies[frame.fif_idx].push_back(std::move(video_frame));
        cmd_buf_dependencies[frame.fif_idx].push_back(std::move(views));
        // for now the pipeline is cached indefinitely, but if we use some
        // strategy like LRU caching, we must ensure that the pipeline live at
        // least as long as command buffer execution
        cmd_buf_dependencies[frame.fif_idx].push_back(std::move(pipeline));
      }

      render_target.end_frame(vk.get_queues(), acq_frame, {});
    }
  }

  vk.get_device().waitIdle();
  return 0;
}
