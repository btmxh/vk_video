import std;
import vulkan_hpp;
import vkvideo;
import vkfw;

#include <cassert>

using namespace vkvideo;
using namespace vkvideo::medias;
using namespace vkvideo::graphics;
using namespace vkvideo::tp;

constexpr i32 sample_rate = 48000;
const auto ch_layout = ffmpeg::ch_layout_stereo;
const ffmpeg::SampleFormat sample_fmt = ffmpeg::SampleFormat::AV_SAMPLE_FMT_FLT;
const portaudio::SampleDataFormat sample_fmt_pa =
    portaudio::SampleDataFormat::FLOAT32;

vkvideo::UniqueAny launch_audio_playback(std::unique_ptr<Audio> audio,
                                         vkvideo::Clock &clock);

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
  VkContext vk{};

  auto surface = vk::raii::SurfaceKHR{
      vk.get_instance(),
      vkfw::createWindowSurface(*vk.get_instance(), *window)};

  auto video = medias::open_video(vk, argv[1]);

  vkr::CommandPool pool{
      vk.get_device(),
      vk::CommandPoolCreateInfo{
          .flags = vk::CommandPoolCreateFlagBits::eTransient |
                   vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
          .queueFamilyIndex =
              static_cast<u32>(vk.get_queues().get_qf_graphics()),
      }};

  vk::raii::SwapchainKHR swapchain = nullptr;
  vk::Format swapchain_format;
  vk::Extent2D swapchain_extent;
  std::vector<vk::Image> swapchain_images;
  std::vector<vk::raii::ImageView> swapchain_image_views;
  std::vector<vk::raii::Semaphore> image_present_sems;

  auto recreate_swapchain = [&]() {
    auto [width, height] = window->getFramebufferSize();
    auto &pd = vk.get_physical_device();
    auto caps = pd.getSurfaceCapabilitiesKHR(*surface);
    auto img_count = std::min(caps.minImageCount + 1, caps.maxImageCount);
    auto formats = pd.getSurfaceFormatsKHR(*surface);
    auto format = formats[0];
    if (auto it = std::find_if(formats.begin(), formats.end(),
                               [&](const auto format) {
                                 return format.format ==
                                            vk::Format::eB8G8R8A8Unorm &&
                                        format.colorSpace ==
                                            vk::ColorSpaceKHR::eSrgbNonlinear;
                               });
        it != formats.end())
      format = *it;

    swapchain = vk::raii::SwapchainKHR{
        vk.get_device(),
        vk::SwapchainCreateInfoKHR{
            .surface = *surface,
            .minImageCount = img_count,
            .imageFormat = swapchain_format = format.format,
            .imageColorSpace = format.colorSpace,
            .imageExtent = swapchain_extent =
                {
                    .width = std::clamp(static_cast<u32>(width),
                                        caps.minImageExtent.width,
                                        caps.maxImageExtent.width),
                    .height = std::clamp(static_cast<u32>(height),
                                         caps.minImageExtent.height,
                                         caps.maxImageExtent.height),
                },
            .imageArrayLayers = 1,
            .imageUsage = vk::ImageUsageFlagBits::eTransferDst |
                          vk::ImageUsageFlagBits::eColorAttachment,
            .imageSharingMode = vk::SharingMode::eExclusive,
            .preTransform = caps.currentTransform,
            .compositeAlpha =
                caps.supportedCompositeAlpha &
                        vk::CompositeAlphaFlagBitsKHR::ePreMultiplied
                    ? vk::CompositeAlphaFlagBitsKHR::ePreMultiplied
                    : vk::CompositeAlphaFlagBitsKHR::eOpaque,
            .presentMode = vk::PresentModeKHR::eFifo,
            .clipped = true,
            .oldSwapchain = *swapchain,
        }};

    swapchain_images = swapchain.getImages();

    swapchain_image_views.clear();
    swapchain_image_views.reserve(swapchain_images.size());

    image_present_sems.clear();
    image_present_sems.reserve(swapchain_images.size());

    for (std::size_t i = 0; i < swapchain_images.size(); ++i) {
      auto image = swapchain_images[i];
      auto name = std::format("swapchain_images[{}]", i);
      vk.set_debug_label(image, name.c_str());
      auto &image_view = swapchain_image_views.emplace_back(
          vk.get_device(),
          vk::ImageViewCreateInfo{
              .image = image,
              .viewType = vk::ImageViewType::e2D,
              .format = swapchain_format,
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
          });
      name = std::format("swapchain_image_views[{}]", i);
      set_debug_label(vk.get_device(), *image_view, name.c_str());

      image_present_sems.emplace_back(vk.get_device(),
                                      vk::SemaphoreCreateInfo{});
    }
  };
  recreate_swapchain();

  // num of frames-in-flight
  static constexpr i32 FIF_CNT = 3;
  std::vector<vk::raii::Semaphore> image_acquire_sems;
  image_acquire_sems.reserve(FIF_CNT);
  for (i32 i = 0; i < FIF_CNT; ++i) {
    auto img_acq_sem = *image_acquire_sems.emplace_back(
        vk.get_device(), vk::SemaphoreCreateInfo{});
    auto name = std::format("image_acquire_sems[{}]", i);
    vk.set_debug_label(img_acq_sem, name.c_str());
  }
  vkr::CommandBuffers cmd_bufs{
      vk.get_device(), vk::CommandBufferAllocateInfo{
                           .commandPool = *pool,
                           .level = vk::CommandBufferLevel::ePrimary,
                           .commandBufferCount = static_cast<u32>(FIF_CNT),
                       }};
  for (i32 i = 0; i < FIF_CNT; ++i) {
    auto name = std::format("gfx_cmd_buf[{}]", i);
    vk.set_debug_label(*cmd_bufs[i], name.c_str());
  }
  std::vector<TimelineSemaphore> cmd_buf_end_sems;
  std::vector<std::vector<UniqueAny>> cmd_buf_dependencies;
  std::vector<u64> cmd_buf_sem_values;
  cmd_buf_end_sems.reserve(FIF_CNT);
  cmd_buf_dependencies.resize(FIF_CNT);
  cmd_buf_sem_values.resize(FIF_CNT);
  for (i32 i = 0; i < FIF_CNT; ++i) {
    auto name = std::format("gfx_cmd_buf_end_sems[{}]", i);
    cmd_buf_end_sems.emplace_back(vk.get_device(), 0, name.c_str());
  }

  VideoPipelineCache pipelines;
  SteadyClock clock;

  std::unique_ptr<medias::Audio> audio = nullptr;
  UniqueAny audio_system{};
  try {
    audio = std::make_unique<medias::AudioStream>(
        argv[1], AudioFormat{
                     .sample_fmt = sample_fmt,
                     .ch_layout = ch_layout,
                     .sample_rate = sample_rate,
                 });
    audio_system = launch_audio_playback(std::move(audio), clock);
  } catch (std::exception &ex) {
    std::println("Error opening audio stream: {}", ex.what());
  }

  for (i32 i = 0; !window->shouldClose(); ++i) {
    vkfw::pollEvents();
    vk.get_temp_pools().garbage_collect();

    auto fif_idx = i % FIF_CNT;
    cmd_buf_end_sems[fif_idx].wait(cmd_buf_sem_values[fif_idx],
                                   std::numeric_limits<i64>::max());
    // once work is done, we can free all dependencies
    cmd_buf_dependencies[fif_idx].clear();
    try {
      auto [result, img_idx] = swapchain.acquireNextImage(
          std::numeric_limits<u64>::max(), image_acquire_sems[fif_idx]);
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
              .color_attachment_format = swapchain_format,
              .pixel_format = video_frame.has_value()
                                  ? video_frame->frame_format
                                  : AV_PIX_FMT_NONE,
          },
          vk.get_device(), FIF_CNT);
      auto views =
          planes | std::ranges::views::transform([&](const auto &plane) {
            return pipeline->create_image_view(vk.get_device(), *plane);
          }) |
          std::ranges::to<std::vector>();
      if (video_frame.has_value()) {
        (*locked_frame_data)
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
                .dstSet = *pipeline->descriptor_sets[fif_idx],
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo = &desc_sampler,
            },
            {});
      }

      // record cmdbuf
      auto &cmd_buf = cmd_bufs[fif_idx];
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
            .image = swapchain_images[img_idx],
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
          .imageView = swapchain_image_views[img_idx],
          .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
          .loadOp = vk::AttachmentLoadOp::eClear,
          .storeOp = vk::AttachmentStoreOp::eStore,
          .clearValue = {vk::ClearColorValue{
              .float32 = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
          }},
      };
      cmd_buf.beginRendering(vk::RenderingInfo{
          .renderArea = {{0, 0}, swapchain_extent},
          .layerCount = 1,
      }
                                 .setColorAttachments(color_attachment));
      if (locked_frame_data.has_value()) {
        auto &data = **locked_frame_data;
        cmd_buf.setViewport(
            0, vk::Viewport{
                   .x = 0,
                   .y = 0,
                   .width = static_cast<float>(swapchain_extent.width),
                   .height = static_cast<float>(swapchain_extent.height),
               });
        cmd_buf.setScissor(0, vk::Rect2D{
                                  .offset = {0, 0},
                                  .extent = swapchain_extent,
                              });
        cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                             pipeline->pipeline);
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   pipeline->pipeline_layout, 0,
                                   *pipeline->descriptor_sets[fif_idx], {});
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
            .image = swapchain_images[img_idx],
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
                .semaphore = image_acquire_sems[fif_idx],
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
            },
        };
        std::vector<vk::SemaphoreSubmitInfo> sig_sem_info{
            {
                .semaphore = cmd_buf_end_sems[fif_idx],
                .value = ++cmd_buf_sem_values[fif_idx],
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
            },
            {
                .semaphore = image_present_sems[img_idx],
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

        cmd_buf_dependencies[fif_idx].push_back(std::move(video_frame));
        cmd_buf_dependencies[fif_idx].push_back(std::move(views));
        // for now the pipeline is cached indefinitely, but if we use some
        // strategy like LRU caching, we must ensure that the pipeline live at
        // least as long as command buffer execution
        cmd_buf_dependencies[fif_idx].push_back(std::move(pipeline));
      }

      {
        auto [p_lock, present_queue] = vk.get_queues().get_graphics_queue();
        auto result = present_queue.presentKHR(
            vk::PresentInfoKHR{}
                .setWaitSemaphores(*image_present_sems[img_idx])
                .setImageIndices(img_idx)
                .setSwapchains(*swapchain));
      }
    } catch (vk::OutOfDateKHRError) {
      recreate_swapchain();
    }
  }

  vk.get_device().waitIdle();
  return 0;
}

template <class T> class WeightedRunningAvg {
public:
  WeightedRunningAvg(i32 max_count, double coeff)
      : coeff{coeff}, max_count{max_count} {}

  std::optional<T> update(T value) {
    cum_value = value + coeff * cum_value;
    if (count < max_count) {
      ++count;
      return std::nullopt;
    }

    return cum_value * (1 - coeff);
  }

  void reset() {
    cum_value = 0;
    count = 0;
  }

private:
  T cum_value = 0;
  i32 count = 0, max_count = 0;
  double coeff = 0.0;
};

// assumes buffer is interleaved
void fill_silence(void *buffer, i32 offset, i32 num_frames) {
  auto sample_size =
      ffmpeg::get_sample_fmt_size(sample_fmt) * ch_layout->nb_channels;
  auto zero_byte =
      sample_fmt == ffmpeg::SampleFormat::AV_SAMPLE_FMT_U8 ? 0x80 : 0;
  auto buffer_u8 = static_cast<u8 *>(buffer);
  std::memset(buffer_u8 + sample_size * offset, zero_byte,
              num_frames * sample_size);
}

vkvideo::UniqueAny launch_audio_playback(std::unique_ptr<Audio> audio,
                                         vkvideo::Clock &clock) {
  namespace pa = tp::portaudio;
  auto system = std::make_unique<pa::AutoSystem>();
  auto &output_device = pa::System::instance().defaultOutputDevice();
  auto delay_avg = std::make_unique<WeightedRunningAvg<i64>>(
      20, std::exp(std::log(1e-2) / 20.0));
  auto sync_resampler = std::make_unique<ffmpeg::AudioResampler>(
      ffmpeg::AudioResampler::create(ch_layout, sample_fmt, sample_rate,
                                     ch_layout, sample_fmt, sample_rate));
  using UserPtrType = std::tuple<Audio &, Clock &, ffmpeg::AudioResampler &,
                                 WeightedRunningAvg<i64> &>;
  auto user_ptr =
      std::make_unique<UserPtrType>(*audio, clock, *sync_resampler, *delay_avg);
  auto audio_stream = std::make_unique<pa::FunCallbackStream>(
      pa::StreamParameters{
          pa::DirectionSpecificStreamParameters::null(),
          pa::DirectionSpecificStreamParameters{
              output_device, ch_layout->nb_channels, sample_fmt_pa, true,
              output_device.defaultLowOutputLatency(), nullptr},
          sample_rate,
          1024,
          0,
      },
      [](auto, void *output, unsigned long num_frames,
         const pa::StreamCallbackTimeInfo *timeInfo, auto, void *userData) {
        static constexpr i64 sec_to_ns = 1e9;
        auto &&[audio, clock, resampler, delay_avg] =
            *static_cast<UserPtrType *>(userData);
        auto out_time = clock.get_time() + timeInfo->outputBufferDacTime -
                        timeInfo->currentTime;
        auto sync_delay = out_time - audio.get_time();
        auto avg_sync_delay = delay_avg.update(sync_delay);

        i32 num_wanted_frames = num_frames;
        if (std::abs(sync_delay) < 1e8) {
          if (avg_sync_delay.has_value() && std::abs(*avg_sync_delay) >= 1e7) {
            num_wanted_frames =
                num_frames + sync_delay * sample_rate / sec_to_ns;
            num_wanted_frames = std::clamp<i32>(
                num_wanted_frames, num_frames * 0.9, num_frames * 1.1);
          }
        } else {
          delay_avg.reset();
          audio.seek(out_time);
          std::println("Seeking audio due to A/V drift...");
        }

        auto frame = ffmpeg::Frame::create();
        frame->sample_rate = sample_rate;
        frame->ch_layout = *ch_layout.get();
        frame->format = sample_fmt;
        frame->nb_samples = num_wanted_frames;
        frame.get_buffer();
        frame.make_writable();
        num_wanted_frames = audio.get_samples(num_frames, frame->data);

        resampler.send(num_wanted_frames, frame->data);
        resampler.set_compensation(
            num_wanted_frames - static_cast<i32>(num_frames), num_frames);
        auto out = static_cast<u8 *>(output);
        auto offset = resampler.recv(num_frames, &out);
        fill_silence(output, offset, num_frames - offset);
        return 0;
      },
      user_ptr.get());
  audio_stream->start();
  return std::tuple{std::move(system),    std::move(audio_stream),
                    std::move(audio),     std::move(sync_resampler),
                    std::move(delay_avg), std::move(user_ptr)};
}
