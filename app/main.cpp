import std;
import vulkan_hpp;
import vkvideo;
import vk_mem_alloc_hpp;

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
      .width = 640,
      .height = 360,
      .fps = {30, 1},
      .sample_rate = 44100,
      .ch_layout = ffmpeg::ch_layout_stereo,
      .sample_format = AV_SAMPLE_FMT_S16,
      .render_output = "output.mkv",
  }};

  auto video = context.open_video(argv[1], {});

  auto &vk = context.get_vulkan();
  vkr::CommandPool pool{
      vk.get_device(),
      vk::CommandPoolCreateInfo{
          .flags = vk::CommandPoolCreateFlagBits::eTransient |
                   vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
          .queueFamilyIndex =
              static_cast<u32>(vk.get_queues().get_qf_graphics()),
      }};

  auto handle_transition = [&](VideoFrame &video_frame) {
    auto planes = video_frame.data->get_planes();
    // TODO: support multi-plane formats
    assert(planes.size() == 1);
    auto &plane = *planes.front();
    auto barrier = plane.image_barrier(
        vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eTransferRead,
        vk::ImageLayout::eTransferSrcOptimal,
        static_cast<u32>(vk.get_queues().get_qf_transfer()));

    std::vector<u32> queues{plane.get_queue_family_idx(),
                            vk.get_queues().get_qf_transfer()};
    std::erase(queues, vk::QueueFamilyIgnored);
    queues.erase(std::unique(queues.begin(), queues.end()), queues.end());
    if (queues.size() == 1 && plane.get_image_layout() == barrier.newLayout)
      return;
    for (auto qfi : queues) {
      auto cmd_buf = vk.get_temp_pools().begin(qfi);
      cmd_buf.begin(vk::CommandBufferBeginInfo{
          .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
      cmd_buf.pipelineBarrier2(
          vk::DependencyInfo{}.setImageMemoryBarriers(barrier));
      cmd_buf.end();
      vk.get_temp_pools().end(std::move(cmd_buf), qfi, {},
                              plane.wait_sem_info(), plane.signal_sem_info());
      plane.commit_image_barrier(barrier);
    }
  };

  auto secs = [&](i64 i) { return i * static_cast<i64>(1e9); };
  std::vector<i64> frame_times = {};
  for (i64 i = 0; i < 3; ++i) {
    frame_times.push_back(i * 1e9);
  }

  std::sort(frame_times.begin(), frame_times.end(), std::greater<>{});

  std::vector<std::pair<i64, VideoFrame>> frames;

  for (const auto frame_time : frame_times) {
    auto frame_opt = video->get_frame(frame_time);
    if (!frame_opt.has_value())
      continue;
    frames.emplace_back(frame_time, frame_opt.value());
  }

  auto [width, height] = frames.front().second.data->get_extent();

  std::vector<std::tuple<i64, std::shared_ptr<TimelineSemaphore>, u64,
                         vma::UniqueBuffer, vma::UniqueAllocation>>
      data;
  for (auto &[frame_time, frame] : frames) {
    handle_transition(frame);

    auto [buffer, buffer_memory] = vk.get_vma_allocator().createBufferUnique(
        vk::BufferCreateInfo{
            .size = static_cast<u32>(width * height),
            .usage = vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
        },
        vma::AllocationCreateInfo{
            .flags = vma::AllocationCreateFlagBits::eMapped |
                     vma::AllocationCreateFlagBits::eHostAccessRandom,
            .requiredFlags = vk::MemoryPropertyFlagBits::eHostVisible,
        });

    auto plane = frame.data->get_planes().front();

    auto qfi = vk.get_queues().get_qf_transfer();
    auto cmd = vk.get_temp_pools().begin(qfi);
    cmd.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    cmd.copyImageToBuffer(
        frame.data->get_planes().front()->get_image(),
        vk::ImageLayout::eTransferSrcOptimal, *buffer,
        vk::BufferImageCopy{
            .bufferOffset = 0,
            .bufferRowLength = static_cast<u32>(width),
            .bufferImageHeight = 0,
            .imageSubresource =
                {
                    .aspectMask = vk::ImageAspectFlagBits::ePlane0,
                    .baseArrayLayer =
                        static_cast<u32>(frame.frame_index.value_or(0)),
                    .layerCount = 1,
                },
            .imageExtent = {static_cast<u32>(width), static_cast<u32>(height),
                            1},
        });
    cmd.end();
    auto [sem, sem_value] = vk.get_temp_pools().end(std::move(cmd), qfi);

    data.emplace_back(frame_time, sem, sem_value, std::move(buffer),
                      std::move(buffer_memory));
  }

  for (auto &[frame_time, sem, sem_value, buffer, buffer_memory] : data) {
    sem->wait(sem_value, std::numeric_limits<i64>::max());

    auto info = vk.get_vma_allocator().getAllocationInfo(*buffer_memory);
    auto out_name = std::format("output_{}.png", frame_time);
    std::span<const u8> pixels{static_cast<const u8 *>(info.pMappedData),
                               static_cast<std::size_t>(width * height)};
    medias::stbi::write_img(out_name, width, height, pixels,
                            tp::ffmpeg::PixelFormat::AV_PIX_FMT_GRAY8);
  }

  context.get_vulkan().get_device().waitIdle();

  return 0;
}
