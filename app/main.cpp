import std;
import vulkan_hpp;
import vkvideo;

#include <cassert>

using namespace vkvideo;
using namespace vkvideo::medias;
using namespace vkvideo::graphics;
using namespace vkvideo::context;
using namespace vkvideo::tp;

struct VideoPipelineInfo {
  std::vector<vk::Format> plane_formats;
  vk::Format color_attachment_format;
  ffmpeg::PixelFormat pixel_format;

  bool operator==(const VideoPipelineInfo &other) const {
    return plane_formats == other.plane_formats &&
           color_attachment_format == other.color_attachment_format &&
           pixel_format == other.pixel_format;
  }

  bool is_yuv() const {
    auto *desc = ffmpeg::get_pix_fmt_desc(pixel_format);
    return desc &&
           !(desc->flags & static_cast<u32>(ffmpeg::PixelFormatFlagBits::eRgb));
  }
};

struct FrameInfoPushConstants {
  float uv_max[2] = {1.0f, 1.0f};
  float frame_index = 0.0f;
};

namespace std {
template <> struct hash<VideoPipelineInfo> {
  std::size_t operator()(const VideoPipelineInfo &info) const {
    std::size_t hash = 0;
    for (auto fmt : info.plane_formats)
      hash = hash * 33 + std::hash<vk::Format>{}(fmt);
    hash = hash * 33 + std::hash<vk::Format>{}(info.color_attachment_format);
    return hash;
  }
};
} // namespace std

struct VideoPipeline {
  vk::raii::SamplerYcbcrConversion yuv_sampler = nullptr;
  vk::raii::Sampler sampler = nullptr;
  vk::raii::DescriptorSetLayout descriptor_set_layout = nullptr;
  std::vector<vk::raii::DescriptorSetLayout> desc_set_layouts;
  vk::raii::PipelineLayout pipeline_layout = nullptr;
  vk::raii::DescriptorPool descriptor_pool = nullptr;
  vk::raii::DescriptorSets descriptor_sets = nullptr;
  vk::raii::Pipeline pipeline = nullptr;

  VideoPipeline(const vk::raii::Device &device, const VideoPipelineInfo &info,
                vkvideo::i32 num_sets) {
    // multiplanar formats not supported
    assert(info.plane_formats.size() <= 1);

    if (info.is_yuv())
      yuv_sampler = vk::raii::SamplerYcbcrConversion{
          device,
          // i dont know much about YUV so im just copying the information from
          // https://themaister.net/blog/2019/12/01/yuv-sampling-in-vulkan-a-niche-and-complicated-feature-vk_khr_ycbcr_sampler_conversion/
          vk::SamplerYcbcrConversionCreateInfo{
              .format = info.plane_formats.front(),
              .ycbcrModel = vk::SamplerYcbcrModelConversion::eYcbcr601,
              .ycbcrRange = vk::SamplerYcbcrRange::eItuFull,
              .components =
                  vk::ComponentMapping{
                      vk::ComponentSwizzle::eR,
                      vk::ComponentSwizzle::eG,
                      vk::ComponentSwizzle::eB,
                      vk::ComponentSwizzle::eA,
                  },
              .xChromaOffset = vk::ChromaLocation::eMidpoint,
              .yChromaOffset = vk::ChromaLocation::eMidpoint,
              .chromaFilter = vk::Filter::eLinear,
              .forceExplicitReconstruction = false,
          }};

    vk::SamplerYcbcrConversionInfo conv_info{.conversion = *yuv_sampler};

    sampler = vk::raii::Sampler{
        device, vk::SamplerCreateInfo{
                    .pNext = *yuv_sampler != nullptr ? &conv_info : nullptr,
                    .magFilter = vk::Filter::eLinear,
                    .minFilter = vk::Filter::eLinear,
                    .mipmapMode = vk::SamplerMipmapMode::eLinear,
                    .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                    .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                    .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                    .mipLodBias = 0.0f,
                    .anisotropyEnable = false,
                    .compareEnable = false,
                    .compareOp = vk::CompareOp::eAlways,
                    .minLod = 0.0f,
                    .maxLod = 0.0f,
                    .borderColor = vk::BorderColor::eFloatOpaqueWhite,
                    .unnormalizedCoordinates = false,
                }};
    vk::DescriptorSetLayoutBinding tex_binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .pImmutableSamplers = &*sampler,
    };

    for (vkvideo::i32 i = 0; i < num_sets; ++i)
      desc_set_layouts.emplace_back(
          device, vk::DescriptorSetLayoutCreateInfo{}.setBindings(tex_binding));
    std::vector<vk::DescriptorSetLayout> desc_set_layouts_non_owning;
    for (auto &l : desc_set_layouts)
      desc_set_layouts_non_owning.push_back(*l);

    vk::PushConstantRange push_const{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(FrameInfoPushConstants),
    };
    pipeline_layout = vk::raii::PipelineLayout{
        device, vk::PipelineLayoutCreateInfo{}
                    .setSetLayouts(desc_set_layouts_non_owning)
                    .setPushConstantRanges(push_const)};
    vk::DescriptorPoolSize desc_pool_size{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = static_cast<vkvideo::u32>(num_sets),
    };
    descriptor_pool = vk::raii::DescriptorPool{
        device,
        vk::DescriptorPoolCreateInfo{
            .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .maxSets = static_cast<vkvideo::u32>(num_sets),
        }
            .setPoolSizes(desc_pool_size)};
    descriptor_sets = vk::raii::DescriptorSets{
        device,
        vk::DescriptorSetAllocateInfo{
            .descriptorPool = descriptor_pool,
            .descriptorSetCount = static_cast<vkvideo::u32>(num_sets),
        }
            .setSetLayouts(desc_set_layouts_non_owning)};
    pipeline = create_pipeline(device, info.color_attachment_format);
  }

  vk::raii::Pipeline create_pipeline(const vk::raii::Device &device,
                                     vk::Format color_attachment_format) {
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    vk::PipelineTessellationStateCreateInfo tesselation{};
    vk::PipelineViewportStateCreateInfo viewport{
        .viewportCount = 1,
        .pViewports = nullptr, // dynamic state
        .scissorCount = 1,
        .pScissors = nullptr, // dynamic state
    };
    vk::PipelineRasterizationStateCreateInfo raster{
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };
    vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = false,
        .alphaToCoverageEnable = false,
        .alphaToOneEnable = false,
    };
    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    vk::PipelineColorBlendStateCreateInfo color_blend{
        .logicOpEnable = false,
    };
    vk::PipelineColorBlendAttachmentState color_blend_attachment{
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eOne,
        .dstAlphaBlendFactor = vk::BlendFactor::eZero,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask =
            vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    color_blend.setAttachments(color_blend_attachment);
    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    auto viewport_dyn_states = {vk::DynamicState::eViewport,
                                vk::DynamicState::eScissor};
    dynamic_state.setDynamicStates(viewport_dyn_states);

    auto load_shader = [&](std::string_view path) {
      std::ifstream file(path.data(), std::ios::binary);
      std::vector<char> data((std::istreambuf_iterator<char>(file)),
                             (std::istreambuf_iterator<char>()));
      return vk::raii::ShaderModule{
          device, vk::ShaderModuleCreateInfo{
                      .codeSize = data.size(),
                      .pCode = reinterpret_cast<vkvideo::u32 *>(data.data()),
                  }};
    };

    auto vertex_shader = load_shader("build/app/fullscreen.vert.spv");
    auto fragment_shader = load_shader("build/app/fullscreen.frag.spv");
    vk::PipelineShaderStageCreateInfo shaders[2] = {
        {
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vertex_shader,
            .pName = "main",
        },
        {
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fragment_shader,
            .pName = "main",
        }};
    vk::PipelineRenderingCreateInfo rendering_info{
        .viewMask = 0,
    };
    rendering_info.setColorAttachmentFormats(color_attachment_format);

    return vk::raii::Pipeline{device, nullptr,
                              vk::GraphicsPipelineCreateInfo{
                                  .pNext = &rendering_info,
                                  .stageCount = std::size(shaders),
                                  .pStages = shaders,
                                  .pVertexInputState = &vertex_input,
                                  .pInputAssemblyState = &input_assembly,
                                  .pTessellationState = &tesselation,
                                  .pViewportState = &viewport,
                                  .pRasterizationState = &raster,
                                  .pMultisampleState = &multisample,
                                  .pDepthStencilState = &depth_stencil,
                                  .pColorBlendState = &color_blend,
                                  .pDynamicState = &dynamic_state,
                                  .layout = pipeline_layout,
                              }};
  }

  vk::raii::ImageView create_image_view(const vk::raii::Device &device,
                                        const VideoFramePlane &plane) {
    vk::SamplerYcbcrConversionInfo conv_info{.conversion = *yuv_sampler};

    return vk::raii::ImageView{
        device, vk::ImageViewCreateInfo{
                    .pNext = *yuv_sampler != nullptr ? &conv_info : nullptr,
                    .image = plane.get_image(),
                    .viewType = vk::ImageViewType::e2DArray,
                    .format = plane.get_format(),
                    .components =
                        vk::ComponentMapping{
                            vk::ComponentSwizzle::eIdentity,
                            vk::ComponentSwizzle::eIdentity,
                            vk::ComponentSwizzle::eIdentity,
                            vk::ComponentSwizzle::eIdentity,
                        },
                    .subresourceRange = plane.get_subresource_range(),
                }};
  }
};

int main(int argc, char *argv[]) {
  namespace vkr = vk::raii;

  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <input.mkv>" << std::endl;
    return 1;
  }

  ffmpeg::Instance ffmpeg;

  Context context{ContextArgs{
      .mode = DisplayMode::Render,
  }};

  auto video = context.open_video(argv[1], {.hwaccel = HWAccel::eOff});

  auto &vk = context.get_vulkan();
  vkr::CommandPool pool{
      vk.get_device(),
      vk::CommandPoolCreateInfo{
          .flags = vk::CommandPoolCreateFlagBits::eTransient |
                   vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
          .queueFamilyIndex =
              static_cast<u32>(vk.get_queues().get_qf_graphics()),
      }};

  auto fif_cnt = vk.num_fifs();
  vkr::CommandBuffers cmd_bufs{
      vk.get_device(), vk::CommandBufferAllocateInfo{
                           .commandPool = *pool,
                           .level = vk::CommandBufferLevel::ePrimary,
                           .commandBufferCount = static_cast<u32>(fif_cnt),
                       }};
  for (i32 i = 0; i < fif_cnt; ++i) {
    auto name = std::format("cmd_buf[{}]", i);
    vk.set_debug_label(*cmd_bufs[i], name.c_str());
  }
  std::vector<TimelineSemaphore> cmd_buf_end_sems;
  std::vector<std::vector<UniqueAny>> cmd_buf_dependencies;
  std::vector<u64> cmd_buf_sem_values;
  cmd_buf_end_sems.reserve(fif_cnt);
  cmd_buf_dependencies.resize(fif_cnt);
  cmd_buf_sem_values.resize(fif_cnt);
  for (i32 i = 0; i < fif_cnt; ++i) {
    auto name = std::format("cmd_buf_end_sems[{}]", i);
    cmd_buf_end_sems.emplace_back(vk.get_device(), 0, name.c_str());
  }

  // FIXME: transition layout validation error on non-HWAccel GPU-assisted
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

  std::unordered_map<VideoPipelineInfo, std::shared_ptr<VideoPipeline>>
      pipelines;

  auto get_pipeline = [&](const VideoPipelineInfo &info) {
    if (auto it = pipelines.find(info); it != pipelines.end()) {
      return it->second;
    } else {
      auto pipeline =
          std::make_shared<VideoPipeline>(vk.get_device(), info, fif_cnt);
      pipelines.emplace(info, pipeline);
      return pipeline;
    }
  };

  context.open_audio(argv[1]);

  for (i32 i = 0; i < 1 && context.alive(); ++i) {
    context.update();

    auto frame = vk.get_render_target().begin_frame();

    cmd_buf_end_sems[frame.fif_idx].wait(cmd_buf_sem_values[frame.fif_idx],
                                         std::numeric_limits<i64>::max());
    // once work is done, we can free all dependencies
    cmd_buf_dependencies[frame.fif_idx].clear();
    if (auto acq_frame_opt =
            frame.acquire_image(std::numeric_limits<i64>::max());
        acq_frame_opt.has_value()) {
      auto acq_frame = acq_frame_opt.value();
      auto video_frame = video->get_frame(0);
      auto locked_frame_data =
          video_frame.transform([](auto &frame) { return frame.data->lock(); });
      auto planes =
          locked_frame_data
              .transform([](auto &data) { return data->get_planes(); })
              .value_or(std::vector<VideoFramePlane *>{});
      auto pipeline = get_pipeline(VideoPipelineInfo{
          .plane_formats = planes |
                           std::ranges::views::transform([](const auto &plane) {
                             return plane->get_format();
                           }) |
                           std::ranges::to<std::vector>(),
          .color_attachment_format = acq_frame.format,
          .pixel_format = video_frame.has_value() ? video_frame->frame_format
                                                  : AV_PIX_FMT_NONE,
      });
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

      // update desc set

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
      if (auto present_layout = vk.get_render_target().present_layout();
          present_layout.has_value()) {
        vk::ImageMemoryBarrier2 sc_img_trans{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eColorAttachmentRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstAccessMask = vk::AccessFlagBits2::eNone,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = *present_layout,
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
                .stageMask = vk::PipelineStageFlagBits2::eNone,
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

      vk.get_render_target().end_frame(vk.get_queues(), acq_frame, {});
    }
  }

  context.get_vulkan().get_device().waitIdle();
  return 0;
}
