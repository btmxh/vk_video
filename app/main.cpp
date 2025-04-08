#include "vkvideo/context/context.hpp"
#include "vkvideo/graphics/tlsem.hpp"
#include "vkvideo/medias/stream.hpp"
#include "vkvideo/medias/video.hpp"

#include <libavutil/hwcontext_vulkan.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_enums.hpp>
#include <vulkan/vulkan_structs.hpp>

#include <chrono>
#include <utility>

int main(int argc, char *argv[]) {
  namespace vkv = vkvideo;
  namespace vkr = vkv::vkr;
  namespace vkff = vkv::ffmpeg;

  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <input.mkv>" << std::endl;
    return 1;
  }

  vkff::Instance ffmpeg;

  vkv::Context context{vkv::ContextArgs{
      .mode = vkv::DisplayMode::Preview,
      .width = 640,
      .height = 360,
      .fps = {30, 1},
      .sample_rate = 44100,
      .ch_layout = vkff::ch_layout_stereo,
      .sample_format = AV_SAMPLE_FMT_S16,
      .render_output = "output.mkv",
  }};

  auto video = context.open_video(argv[1], {.hwaccel = vkv::HWAccel::eOff,
                                            .mode = vkv::DecodeMode::eReadAll});
  using namespace std::chrono_literals;
  auto to_ns = [](auto x) {
    return static_cast<vkv::i64>(std::chrono::nanoseconds{x}.count());
  };
  auto frame = video->get_frame(to_ns(10ns));
  assert(frame);
  std::cout << frame->frame_index.value() << std::endl;

  auto &vk = context.get_vulkan();
  vkr::CommandPool pool{
      vk.get_device(),
      vk::CommandPoolCreateInfo{
          .flags = vk::CommandPoolCreateFlagBits::eTransient |
                   vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
          .queueFamilyIndex = static_cast<vkv::u32>(vk.get_qf_graphics()),
      }};

  auto fif_cnt = vk.get_swapchain_ctx().num_fifs();
  vkr::CommandBuffers cmd_bufs{
      vk.get_device(), vk::CommandBufferAllocateInfo{
                           .commandPool = *pool,
                           .level = vk::CommandBufferLevel::ePrimary,
                           .commandBufferCount = static_cast<vkv::u32>(fif_cnt),
                       }};
  std::vector<vkv::TimelineSemaphore> cmd_buf_sems;
  std::vector<vkr::Semaphore> present_sems;
  cmd_buf_sems.reserve(fif_cnt);
  present_sems.reserve(fif_cnt);
  for (vkv::i32 i = 0; i < fif_cnt; ++i) {
    cmd_buf_sems.emplace_back(vk.get_device(), i);
    present_sems.emplace_back(vk.get_device(), vk::SemaphoreCreateInfo{});
  }

  video->wait_for_load(INT64_MAX);

  {
    auto video_frame = video->get_frame(0);
    assert(video_frame.has_value() && video_frame->data->planes.size() == 1);
    auto &plane = video_frame->data->planes.front();

    // handle queue ownership transfer...
    if (plane.queue_family_idx != vk.get_qf_graphics() &&
        plane.queue_family_idx != vk::QueueFamilyIgnored) {
      auto cmd_buf = vk.get_temp_pools().begin(plane.queue_family_idx);
      cmd_buf.begin(vk::CommandBufferBeginInfo{
          .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
      vk::ImageMemoryBarrier2 barrier{
          .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
          .srcAccessMask = vk::AccessFlagBits2::eNone,
          .dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
          .dstAccessMask = vk::AccessFlagBits2::eNone,
          .oldLayout = plane.layout,
          .newLayout = vk::ImageLayout::eTransferSrcOptimal,
          .srcQueueFamilyIndex = plane.queue_family_idx,
          .dstQueueFamilyIndex = static_cast<vkv::u32>(vk.get_qf_graphics()),
          .image = plane.image,
          .subresourceRange = {
              .aspectMask = vk::ImageAspectFlagBits::eColor,
              .levelCount = 1,
              .layerCount = static_cast<vkv::u32>(plane.num_layers),
          }};
      cmd_buf.pipelineBarrier2(
          vk::DependencyInfo{}.setImageMemoryBarriers(barrier));
      cmd_buf.end();
      vk::SemaphoreSubmitInfo wait_sem{
          .semaphore = plane.semaphore,
          .value = plane.semaphore_value,
          .stageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
      };
      auto [rel_sem, rel_sem_value] = vk.get_temp_pools().end(
          std::move(cmd_buf), plane.queue_family_idx, {}, wait_sem);

      rel_sem->wait(rel_sem_value, UINT64_MAX);

      cmd_buf = vk.get_temp_pools().begin(vk.get_qf_graphics());
      cmd_buf.begin(vk::CommandBufferBeginInfo{
          .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
      cmd_buf.pipelineBarrier2(
          vk::DependencyInfo{}.setImageMemoryBarriers(barrier));
      cmd_buf.end();

      vk::SemaphoreSubmitInfo wait_sem2{
          .semaphore = *rel_sem,
          .value = rel_sem_value,
          .stageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
      };
      auto [acq_sem, acq_sem_value] = vk.get_temp_pools().end(
          std::move(cmd_buf), vk.get_qf_graphics(), rel_sem, wait_sem2);

      plane.semaphore = **acq_sem;
      plane.semaphore_value = acq_sem_value;
      plane.layout = vk::ImageLayout::eTransferSrcOptimal;
      plane.queue_family_idx = vk.get_qf_graphics();
      video_frame->data->buf =
          std::pair{std::move(video_frame->data->buf), acq_sem};
    } else if (plane.layout != vk::ImageLayout::eTransferSrcOptimal) {
      // TODO: handle this case similarly
      std::unreachable();
    }
  }

  auto start_time = std::chrono::high_resolution_clock::now();
  auto prev_time = start_time;

  for (vkv::i32 i = 0; context.alive(); ++i) {
    context.update();

    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = now - prev_time;
    prev_time = now;

    auto elapsed_from_start = now - start_time;

    auto &present = context.get_vulkan().get_swapchain_ctx();
    auto frame = present.begin_frame();
    if (frame.frame_idx % 100 == 0) {
      std::cout << elapsed.count() << '\n';
      std::cout << "current time" << elapsed_from_start.count() << '\n';
    }

    cmd_buf_sems[frame.fif_idx].wait(frame.frame_idx, INT64_MAX);
    if (auto image_opt = frame.acquire_image(UINT64_MAX);
        image_opt.has_value()) {
      auto [image_idx, image] = image_opt.value();
      // record cmdbuf
      auto &cmd_buf = cmd_bufs[frame.fif_idx];
      cmd_buf.begin(vk::CommandBufferBeginInfo{
          .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

      // transition: eUndefined -> eTransferDstOptimal
      {
        vk::ImageMemoryBarrier2 sc_img_trans{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1,
            }};
        cmd_buf.pipelineBarrier2(
            vk::DependencyInfo{}.setImageMemoryBarriers(sc_img_trans));
      }
      video->seek(0);
      auto video_frame = video->get_frame(elapsed_from_start.count() % (vkv::i64)1e9);
      assert(video_frame.has_value() && video_frame->data->planes.size() == 1);
      auto &plane = video_frame->data->planes.front();
      if (plane.layout != vk::ImageLayout::eTransferSrcOptimal ||
          (plane.queue_family_idx != vk.get_qf_graphics() &&
           plane.queue_family_idx != vk::QueueFamilyIgnored)) {
        auto trans_cmd_buf = vk.get_temp_pools().begin(plane.queue_family_idx);
        trans_cmd_buf.begin(vk::CommandBufferBeginInfo{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        // image transition on another command buffer
        vk::ImageMemoryBarrier2 frame_img_trans{
            .srcStageMask = plane.stage,
            .srcAccessMask = plane.access,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = plane.layout,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = plane.queue_family_idx,
            .dstQueueFamilyIndex = static_cast<vkv::u32>(vk.get_qf_graphics()),
            .image = plane.image,
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = static_cast<vkv::u32>(plane.num_layers),
                },
        };

        trans_cmd_buf.pipelineBarrier2(
            vk::DependencyInfo{}.setImageMemoryBarriers(frame_img_trans));
        trans_cmd_buf.end();
        auto [trans_sem, trans_sem_value] = vk.get_temp_pools().end(
            std::move(trans_cmd_buf), plane.queue_family_idx);

        // after transition, update the plane
        plane.layout = vk::ImageLayout::eTransferSrcOptimal;
        plane.stage = vk::PipelineStageFlagBits2::eBlit;
        plane.access = vk::AccessFlagBits2::eTransferRead;
        plane.semaphore = *trans_sem;
        plane.semaphore_value = trans_sem_value;
        plane.queue_family_idx = vk.get_qf_graphics();

        // transfer ownership of `trans_sem` to the frame,
        // TODO: if multiple image transitions are needed,
        //       make it possible to remove semaphores
        //       from the buf
        auto new_buf = std::pair{std::move(video_frame->data->buf), trans_sem};
        video_frame->data->buf = std::move(new_buf);

        // next time, transition should not happen as the layout and
        // queue_family_idx should be both updated
      }
      // blit
      {
        std::cout << video_frame->frame_index.value() << std::endl;
        vk::ImageBlit2 region{
            .srcSubresource =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseArrayLayer = static_cast<vkv::u32>(
                        video_frame->frame_index.value_or(0)),
                    .layerCount = 1,
                },
            .dstSubresource =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .layerCount = 1,
                },
        };

        region.srcOffsets[1] = vk::Offset3D{video_frame->data->width,
                                            video_frame->data->height, 1};
        region.dstOffsets[1] = vk::Offset3D{frame.width, frame.height, 1};
        cmd_buf.blitImage2(vk::BlitImageInfo2{
            .srcImage = plane.image,
            .srcImageLayout = plane.layout,
            .dstImage = image,
            .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
            .filter = vk::Filter::eLinear,
        }
                               .setRegions(region));
      }
      // transition: eTransferDstOptimal -> ePresentSrcKHR
      {
        vk::ImageMemoryBarrier2 sc_img_trans{
            .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
            .dstAccessMask = vk::AccessFlagBits2::eNone,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = image,
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
        vk::SemaphoreSubmitInfo wait_sem_info[2]{
            {
                .semaphore = plane.semaphore,
                .value = plane.semaphore_value,
                .stageMask = plane.stage,
            },
            {
                .semaphore = frame.image_acq_sem,
                .stageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            },
        };
        vk::SemaphoreSubmitInfo sig_sem_info[2]{
            {
                .semaphore = cmd_buf_sems[frame.fif_idx],
                .value = static_cast<vkv::u64>(frame.frame_idx + fif_cnt),
                .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
            },
            {
                .semaphore = present_sems[frame.fif_idx],
                .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
            },
        };

        vk.get_graphics_queue().submit2(
            vk::SubmitInfo2{}
                .setCommandBufferInfos(cmd_buf_info)
                .setWaitSemaphoreInfos(wait_sem_info)
                .setSignalSemaphoreInfos(sig_sem_info));
      }

      vk::Semaphore wait_sem = *present_sems[frame.fif_idx];
      present.end_frame(image_idx, wait_sem);
    }
  }

  context.get_vulkan().get_device().waitIdle();

  return 0;
}
