module;
#include <cmrc/cmrc.hpp>
#include <shaderc/shaderc.hpp>

#include <cassert>
CMRC_DECLARE(vkvideo_shaders);

extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}
export module vkvideo.medias:hwrescale;

import std;
import vulkan_hpp;
import vkvideo.core;
import vkvideo.graphics;
import vkvideo.third_party;

export namespace vkvideo::medias {

class HwVideoRescaler {
public:
  virtual ~HwVideoRescaler() = default;

  virtual vk::ImageLayout input_image_layout() = 0;
  virtual std::vector<vk::ImageLayout> output_image_layout() = 0;
  virtual vk::PipelineStageFlags2 pipeline_stage_flags() = 0;
  virtual vk::AccessFlags2 input_access_flags() = 0;
  virtual vk::AccessFlags2 output_access_flags() = 0;

  virtual UniqueAny bind_images(vk::raii::Device &device, vk::Image source,
                                const std::span<const vk::Image> &target) = 0;
  virtual void rescale(vk::raii::CommandBuffer &cmd, i32 width, i32 height) = 0;
};

// compute-shader-based video rescaling from RGBA to YUV formats
// Notes:
// - Input format MUST be RGBA
// - YUV rescaling is done via a (currently unoptimized) compute shader.
//   Theoretically, it is possible to optimize this shader based on the input
//   format, therefore this class assumes that the GLSL compiler can do this
//   and pass the input format using macros.
// - Output frame is assumed to be a FFmpeg-backed AVVkFrame.
class YuvVideoRescaler : public HwVideoRescaler {
private:
  static std::span<const vk::Format>
  null_terminated_format_list(const vk::Format *p) {
    auto last = p;
    while (*last != vk::Format::eUndefined)
      ++last;
    return {p, last};
  }
  static std::string
  build_color_format_string(tp::ffmpeg::PixelFormat out_format) {
    auto desc = tp::ffmpeg::get_pix_fmt_desc(out_format);
    std::array<i32, 4> plane, offset;
    plane.fill(0);
    offset.fill(0);
    for (i32 i = 0; i < desc->nb_components; ++i) {
      plane[i] = desc->comp[i].plane;
      offset[i] = desc->comp[i].offset;
    }
    auto vkfmt =
        reinterpret_cast<const vk::Format *>(av_vkfmt_from_pixfmt(out_format));
    assert(vkfmt);
    // TODO: check if this works for all
    i32 y_per_block =
        std::max(1, null_terminated_format_list(vkfmt).size() != 1
                        ? 1
                        : vk::blockSize(*vkfmt) / desc->comp[0].step);
    return std::format("{}, {}, {},", desc->nb_components,
                       *std::ranges::max_element(plane) + 1, 1) +
           std::format("ivec2({}, {}),", desc->log2_chroma_w,
                       desc->log2_chroma_h) +
           std::format("ivec4({}, {}, {}, {}),", plane[0], plane[1], plane[2],
                       plane[3]) +
           std::format("ivec4({}, {}, {}, {})", offset[0], offset[1], offset[2],
                       offset[3]);
  }

  static std::vector<u32>
  compile_rescaling_shader(tp::ffmpeg::PixelFormat out_format) {
    auto fs = cmrc::vkvideo_shaders::get_filesystem();
    auto hwrescale = fs.open("medias/hwrescale.comp");

    shaderc::Compiler glslc;
    shaderc::CompileOptions opts;

    opts.AddMacroDefinition("COLOR_FORMAT",
                            build_color_format_string(out_format));
    opts.SetOptimizationLevel(shaderc_optimization_level_performance);

    auto result = glslc.CompileGlslToSpv(hwrescale.begin(), hwrescale.size(),
                                         shaderc_compute_shader,
                                         "medias/hwrescale.comp", opts);
    if (!std::ranges::all_of(result.GetErrorMessage(),
                             [](auto c) { return std::isspace(c); }))
      std::println("HwRescaling shader message: {}", result.GetErrorMessage());
    assert(result.GetCompilationStatus() == shaderc_compilation_status_success);

    return std::vector<u32>{result.begin(), result.end()};
  }

public:
  YuvVideoRescaler(vk::raii::Device &device, tp::ffmpeg::PixelFormat out_format)
      : pixel_format{out_format}, vk_format_list{null_terminated_format_list(
                                      reinterpret_cast<const vk::Format *>(
                                          av_vkfmt_from_pixfmt(out_format)))} {
    auto code = compile_rescaling_shader(out_format);
    vk::raii::ShaderModule module{device,
                                  vk::ShaderModuleCreateInfo{
                                      .codeSize = code.size() * sizeof(code[0]),
                                      .pCode = code.data(),
                                  }};
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = vk::DescriptorType::eStorageImage;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
    }
    vk::PushConstantRange push_const_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(i32) * 2,
    };
    desc_set_layout = vk::raii::DescriptorSetLayout{
        device, vk::DescriptorSetLayoutCreateInfo{}.setBindings(bindings)};
    pipeline_layout = vk::raii::PipelineLayout{
        device, vk::PipelineLayoutCreateInfo{}
                    .setSetLayouts(*desc_set_layout)
                    .setPushConstantRanges(push_const_range)};
    pipeline = vk::raii::Pipeline{
        device, nullptr,
        vk::ComputePipelineCreateInfo{
            .stage =
                {
                    .stage = vk::ShaderStageFlagBits::eCompute,
                    .module = module,
                    .pName = "main",
                },
            .layout = *pipeline_layout,
        }};
    vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageImage,
                                     bindings.size()};
    desc_pool = vk::raii::DescriptorPool{
        device,
        vk::DescriptorPoolCreateInfo{
            .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .maxSets = 1,
        }
            .setPoolSizes(pool_size)};
    vk::raii::DescriptorSets desc_sets{device,
                                       vk::DescriptorSetAllocateInfo{
                                           .descriptorPool = *desc_pool,
                                       }
                                           .setSetLayouts(*desc_set_layout)};
    desc_set = std::move(desc_sets[0]);
  }

  std::vector<vk::raii::ImageView>
  create_output_views(vk::raii::Device &device,
                      const std::span<const vk::Image> &planes) {
    assert(planes.size() >= 1);
    std::vector<vk::raii::ImageView> views;
    auto get_image = [&](i32 i) {
      return planes.size() == 1 ? planes[0] : planes[i];
    };
    auto get_aspect_mask = [&](i32 i) {
      assert(planes.size() >= 1);
      // multi-plane image represented as multi-images
      if (planes.size() > 1)
        return vk::ImageAspectFlagBits::eColor;
      // multi-plane image represented as one image
      if (vk_format_list.size() > 1)
        switch (i) {
        case 0:
          return vk::ImageAspectFlagBits::ePlane0;
        case 1:
          return vk::ImageAspectFlagBits::ePlane1;
        case 2:
          return vk::ImageAspectFlagBits::ePlane2;
        }
      // single-plane image represented as one image
      return vk::ImageAspectFlagBits::eColor;
    };
    for (i32 i = 0; i < vk_format_list.size(); ++i) {
      views.emplace_back(device, vk::ImageViewCreateInfo{
                                     .image = get_image(i),
                                     .viewType = vk::ImageViewType::e2D,
                                     .format = vk_format_list[i],
                                     .components =
                                         {
                                             vk::ComponentSwizzle::eIdentity,
                                             vk::ComponentSwizzle::eIdentity,
                                             vk::ComponentSwizzle::eIdentity,
                                             vk::ComponentSwizzle::eIdentity,
                                         },
                                     .subresourceRange = {
                                         .aspectMask = get_aspect_mask(i),
                                         .baseMipLevel = 0,
                                         .levelCount = 1,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1,
                                     }});
    }
    return views;
  }

  vk::ImageLayout input_image_layout() override {
    return vk::ImageLayout::eGeneral;
  }

  std::vector<vk::ImageLayout> output_image_layout() override { return {}; }

  vk::PipelineStageFlags2 pipeline_stage_flags() override {
    return vk::PipelineStageFlagBits2::eComputeShader;
  }

  vk::AccessFlags2 input_access_flags() override {
    return vk::AccessFlagBits2::eShaderStorageRead;
  }

  vk::AccessFlags2 output_access_flags() override {
    return vk::AccessFlagBits2::eShaderStorageWrite;
  }

  UniqueAny bind_images(vk::raii::Device &device, vk::Image source,
                        const std::span<const vk::Image> &target) override {
    static constexpr std::size_t num_images = 5;
    std::array<vk::DescriptorImageInfo, num_images> image_infos;
    std::array<vk::WriteDescriptorSet, num_images> write_ops;
    std::array<vk::ImageView, num_images> image_views;
    std::vector<vk::raii::ImageView> all_image_views =
        create_output_views(device, target);
    // target views
    for (i32 i = 0; i < num_images - 1; ++i)
      image_views[i] = all_image_views[i < all_image_views.size() ? i : 0];
    // source view
    image_views.back() = all_image_views.emplace_back(
        device, vk::ImageViewCreateInfo{
                    .image = source,
                    .viewType = vk::ImageViewType::e2D,
                    .format = vk::Format::eR32G32B32A32Sfloat,
                    .components =
                        {
                            vk::ComponentSwizzle::eIdentity,
                            vk::ComponentSwizzle::eIdentity,
                            vk::ComponentSwizzle::eIdentity,
                            vk::ComponentSwizzle::eIdentity,
                        },
                    .subresourceRange = {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .levelCount = 1,
                        .layerCount = 1,
                    }});

    u32 i = 0;
    for (auto &&[image_view, image_info, write_op] :
         std::views::zip(image_views, image_infos, write_ops)) {
      image_info = vk::DescriptorImageInfo{
          .imageView = image_view,
          .imageLayout = vk::ImageLayout::eGeneral,
      };
      write_op = vk::WriteDescriptorSet{
          .dstSet = *desc_set,
          .dstBinding = i++,
          .dstArrayElement = 0,
          .descriptorCount = 1,
          .descriptorType = vk::DescriptorType::eStorageImage,
          .pImageInfo = &image_info,
      };
    }

    device.updateDescriptorSets(write_ops, {});
    return all_image_views;
  }

  void rescale(vk::raii::CommandBuffer &cmd, i32 width, i32 height) override {
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0,
                           *desc_set, {});
    cmd.pushConstants<i32>(*pipeline_layout, vk::ShaderStageFlagBits::eCompute,
                           0, std::array<i32, 2>{width, height});
    cmd.dispatch(AV_CEIL_RSHIFT(width, log2_chroma[0]),
                 AV_CEIL_RSHIFT(height, log2_chroma[1]), 1);
  }

private:
  vk::raii::DescriptorSetLayout desc_set_layout = nullptr;
  vk::raii::DescriptorPool desc_pool = nullptr;
  vk::raii::PipelineLayout pipeline_layout = nullptr;
  vk::raii::Pipeline pipeline = nullptr;
  vk::raii::DescriptorSet desc_set = nullptr;
  std::array<i32, 2> log2_chroma;
  tp::ffmpeg::PixelFormat pixel_format;
  std::span<const vk::Format> vk_format_list;
};

class RgbVideoRescaler : public HwVideoRescaler {
public:
  RgbVideoRescaler() = default;

  vk::ImageLayout input_image_layout() override {
    return vk::ImageLayout::eTransferSrcOptimal;
  }

  std::vector<vk::ImageLayout> output_image_layout() override {
    return {vk::ImageLayout::eTransferDstOptimal};
  }

  vk::PipelineStageFlags2 pipeline_stage_flags() override {
    return vk::PipelineStageFlagBits2::eBlit;
  }

  vk::AccessFlags2 input_access_flags() override {
    return vk::AccessFlagBits2::eTransferRead;
  }

  vk::AccessFlags2 output_access_flags() override {
    return vk::AccessFlagBits2::eTransferWrite;
  }

  UniqueAny bind_images(vk::raii::Device &device, vk::Image source,
                        const std::span<const vk::Image> &target) override {
    assert(target.size() == 1);
    this->source = source;
    this->target = target.front();
    return {};
  }

  void rescale(vk::raii::CommandBuffer &cmd, i32 width, i32 height) override {
    vk::ImageBlit2 region{
        .srcSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .layerCount = 1,
            },
        .srcOffsets = std::array<vk::Offset3D, 2>{{
            {0, 0, 0},
            {width, height, 1},
        }},
        .dstSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .layerCount = 1,
            },
        .dstOffsets = std::array<vk::Offset3D, 2>{{
            {0, 0, 0},
            {width, height, 1},
        }},
    };
    cmd.blitImage2(vk::BlitImageInfo2{
        .srcImage = source,
        .srcImageLayout = input_image_layout(),
        .dstImage = target,
        .dstImageLayout = output_image_layout().front(),
        .filter = vk::Filter::eLinear,
    }
                       .setRegions(region));
  }

private:
  vk::Image source = nullptr;
  vk::Image target;
};

void get_cached_hw_rescaler(std::unique_ptr<HwVideoRescaler> &rescaler,
                            vk::raii::Device &device,
                            tp::ffmpeg::PixelFormat out_format) {
  bool rgb = tp::ffmpeg::get_pix_fmt_desc(out_format)->flags &
             static_cast<unsigned>(tp::ffmpeg::PixelFormatFlagBits::eRgb);
  if (rescaler.get() && dynamic_cast<RgbVideoRescaler *>(rescaler.get()) && rgb)
    return;
  if (rescaler.get() && dynamic_cast<YuvVideoRescaler *>(rescaler.get()) &&
      !rgb)
    return;

  if (rgb) {
    rescaler = std::make_unique<RgbVideoRescaler>();
  } else {
    rescaler = std::make_unique<YuvVideoRescaler>(device, out_format);
  }
}

} // namespace vkvideo::medias
