module;
#include <cassert>
export module vkvideo.medias:pipeline;

import std;
import vulkan_hpp;
import vkvideo.core;
import vkvideo.third_party;
import :video_frame;

export namespace vkvideo::medias {

struct VideoPipelineInfo {
  std::vector<vk::Format> plane_formats;
  vk::Format color_attachment_format;
  tp::ffmpeg::PixelFormat pixel_format;

  bool operator==(const VideoPipelineInfo &other) const {
    return plane_formats == other.plane_formats &&
           color_attachment_format == other.color_attachment_format &&
           pixel_format == other.pixel_format;
  }

  bool is_yuv() const {
    auto *desc = tp::ffmpeg::get_pix_fmt_desc(pixel_format);
    return desc && !(desc->flags &
                     static_cast<u32>(tp::ffmpeg::PixelFormatFlagBits::eRgb));
  }
};

struct FrameInfoPushConstants {
  float uv_max[2] = {1.0f, 1.0f};
  float frame_index = 0.0f;
};

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
                i32 num_sets) {
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

    auto vertex_shader = load_shader("build/src/fullscreen.vert.spv");
    auto fragment_shader = load_shader("build/src/fullscreen.frag.spv");
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
} // namespace vkvideo::medias

namespace std {
template <> struct hash<vkvideo::medias::VideoPipelineInfo> {
  std::size_t operator()(const vkvideo::medias::VideoPipelineInfo &info) const {
    std::size_t hash = 0;
    for (auto fmt : info.plane_formats)
      hash = hash * 33 + std::hash<vk::Format>{}(fmt);
    hash = hash * 33 + std::hash<vk::Format>{}(info.color_attachment_format);
    return hash;
  }
};
} // namespace std

export namespace vkvideo::medias {
class VideoPipelineCache {
public:
  std::shared_ptr<VideoPipeline> get(const VideoPipelineInfo &info,
                                     vk::raii::Device &device, i32 fif_cnt) {
    if (auto it = pipelines.find(info); it != pipelines.end()) {
      return it->second;
    } else {
      auto pipeline = std::make_shared<VideoPipeline>(device, info, fif_cnt);
      pipelines.emplace(info, pipeline);
      return pipeline;
    }
  }

private:
  std::unordered_map<VideoPipelineInfo, std::shared_ptr<VideoPipeline>>
      pipelines;
};

} // namespace vkvideo::medias
