// #include "vkvideo/context/context.hpp"
// #include "vkvideo/graphics/tlsem.hpp"
// #include "vkvideo/graphics/video_frame.hpp"
// #include "vkvideo/medias/stream.hpp"
// #include "vkvideo/medias/video.hpp"
//
// #include <libavutil/hwcontext_vulkan.h>
// #include <vulkan/vulkan.hpp>
// #include <vulkan/vulkan_enums.hpp>
// #include <vulkan/vulkan_handles.hpp>
// #include <vulkan/vulkan_raii.hpp>
// #include <vulkan/vulkan_structs.hpp>
//
// #include <chrono>
// #include <fstream>
// #include <utility>
//
// extern "C" {
// #include <libavutil/pixdesc.h>
// }
//
// struct VideoPipelineInfo {
//   std::vector<vk::Format> plane_formats;
//   vk::Format color_attachment_format;
//   vkvideo::ffmpeg::PixelFormat pixel_format;
//
//   bool operator==(const VideoPipelineInfo &other) const {
//     return plane_formats == other.plane_formats &&
//            color_attachment_format == other.color_attachment_format &&
//            pixel_format == other.pixel_format;
//   }
//
//   bool is_yuv() const {
//     auto *desc = av_pix_fmt_desc_get(pixel_format);
//     return desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
//   }
// };
//
// struct FrameInfoPushConstants {
//   float uv_max[2] = {1.0f, 1.0f};
//   float frame_index = 0.0f;
// };
//
// namespace std {
// template <> struct hash<VideoPipelineInfo> {
//   std::size_t operator()(const VideoPipelineInfo &info) const {
//     std::size_t hash = 0;
//     for (auto fmt : info.plane_formats)
//       hash = hash * 33 + std::hash<vk::Format>{}(fmt);
//     hash = hash * 33 + std::hash<vk::Format>{}(info.color_attachment_format);
//     return hash;
//   }
// };
// } // namespace std
//
// struct VideoPipeline {
//   vk::raii::SamplerYcbcrConversion yuv_sampler = nullptr;
//   vk::raii::Sampler sampler = nullptr;
//   vk::raii::DescriptorSetLayout descriptor_set_layout = nullptr;
//   std::vector<vk::raii::DescriptorSetLayout> desc_set_layouts;
//   vk::raii::PipelineLayout pipeline_layout = nullptr;
//   vk::raii::DescriptorPool descriptor_pool = nullptr;
//   vk::raii::DescriptorSets descriptor_sets = nullptr;
//   vk::raii::Pipeline pipeline = nullptr;
//
//   VideoPipeline(const vk::raii::Device &device, const VideoPipelineInfo
//   &info,
//                 vkvideo::i32 num_sets) {
//     // multiplanar formats not supported
//     assert(info.plane_formats.size() <= 1);
//
//     if (info.is_yuv())
//       yuv_sampler = vk::raii::SamplerYcbcrConversion{
//           // i dont know much about YUV so im just copying the information
//           from
//           //
//           https://themaister.net/blog/2019/12/01/yuv-sampling-in-vulkan-a-niche-and-complicated-feature-vk_khr_ycbcr_sampler_conversion/
//           device, vk::SamplerYcbcrConversionCreateInfo{
//                       .format = info.plane_formats.front(),
//                       .ycbcrModel =
//                       vk::SamplerYcbcrModelConversion::eYcbcr601, .ycbcrRange
//                       = vk::SamplerYcbcrRange::eItuFull, .components =
//                           vk::ComponentMapping{
//                               vk::ComponentSwizzle::eR,
//                               vk::ComponentSwizzle::eG,
//                               vk::ComponentSwizzle::eB,
//                               vk::ComponentSwizzle::eA,
//                           },
//                       .xChromaOffset = vk::ChromaLocation::eMidpoint,
//                       .yChromaOffset = vk::ChromaLocation::eMidpoint,
//                       .chromaFilter = vk::Filter::eLinear,
//                       .forceExplicitReconstruction = false,
//                   }};
//
//     vk::SamplerYcbcrConversionInfo conv_info{.conversion = *yuv_sampler};
//
//     sampler = vk::raii::Sampler{
//         device, vk::SamplerCreateInfo{
//                     .pNext = yuv_sampler != nullptr ? &conv_info : nullptr,
//                     .magFilter = vk::Filter::eLinear,
//                     .minFilter = vk::Filter::eLinear,
//                     .mipmapMode = vk::SamplerMipmapMode::eLinear,
//                     .addressModeU = vk::SamplerAddressMode::eClampToEdge,
//                     .addressModeV = vk::SamplerAddressMode::eClampToEdge,
//                     .addressModeW = vk::SamplerAddressMode::eClampToEdge,
//                     .mipLodBias = 0.0f,
//                     .anisotropyEnable = false,
//                     .compareEnable = false,
//                     .compareOp = vk::CompareOp::eAlways,
//                     .minLod = 0.0f,
//                     .maxLod = 0.0f,
//                     .borderColor = vk::BorderColor::eFloatOpaqueWhite,
//                     .unnormalizedCoordinates = false,
//                 }};
//     vk::DescriptorSetLayoutBinding tex_binding{
//         .binding = 0,
//         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
//         .descriptorCount = 1,
//         .stageFlags = vk::ShaderStageFlagBits::eFragment,
//         .pImmutableSamplers = &*sampler,
//     };
//
//     for (vkvideo::i32 i = 0; i < num_sets; ++i)
//       desc_set_layouts.emplace_back(
//           device,
//           vk::DescriptorSetLayoutCreateInfo{}.setBindings(tex_binding));
//     std::vector<vk::DescriptorSetLayout> desc_set_layouts_non_owning;
//     for (auto &l : desc_set_layouts)
//       desc_set_layouts_non_owning.push_back(*l);
//
//     vk::PushConstantRange push_const{
//         .stageFlags = vk::ShaderStageFlagBits::eFragment,
//         .offset = 0,
//         .size = sizeof(FrameInfoPushConstants),
//     };
//     pipeline_layout = vk::raii::PipelineLayout{
//         device, vk::PipelineLayoutCreateInfo{}
//                     .setSetLayouts(desc_set_layouts_non_owning)
//                     .setPushConstantRanges(push_const)};
//     vk::DescriptorPoolSize desc_pool_size{
//         .type = vk::DescriptorType::eCombinedImageSampler,
//         .descriptorCount = static_cast<vkvideo::u32>(num_sets),
//     };
//     descriptor_pool = vk::raii::DescriptorPool{
//         device,
//         vk::DescriptorPoolCreateInfo{
//             .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
//             .maxSets = static_cast<vkvideo::u32>(num_sets),
//         }
//             .setPoolSizes(desc_pool_size)};
//     descriptor_sets = vk::raii::DescriptorSets{
//         device,
//         vk::DescriptorSetAllocateInfo{
//             .descriptorPool = descriptor_pool,
//             .descriptorSetCount = static_cast<vkvideo::u32>(num_sets),
//         }
//             .setSetLayouts(desc_set_layouts_non_owning)};
//     pipeline = create_pipeline(device, info.color_attachment_format);
//   }
//
//   vk::raii::Pipeline create_pipeline(const vk::raii::Device &device,
//                                      vk::Format color_attachment_format) {
//     vk::PipelineVertexInputStateCreateInfo vertex_input{};
//     vk::PipelineInputAssemblyStateCreateInfo input_assembly{
//         .topology = vk::PrimitiveTopology::eTriangleList,
//     };
//     vk::PipelineTessellationStateCreateInfo tesselation{};
//     vk::PipelineViewportStateCreateInfo viewport{
//         .viewportCount = 1,
//         .pViewports = nullptr, // dynamic state
//         .scissorCount = 1,
//         .pScissors = nullptr, // dynamic state
//     };
//     vk::PipelineRasterizationStateCreateInfo raster{
//         .depthClampEnable = false,
//         .rasterizerDiscardEnable = false,
//         .polygonMode = vk::PolygonMode::eFill,
//         .cullMode = vk::CullModeFlagBits::eNone,
//         .depthBiasEnable = false,
//         .lineWidth = 1.0f,
//     };
//     vk::PipelineMultisampleStateCreateInfo multisample{
//         .rasterizationSamples = vk::SampleCountFlagBits::e1,
//         .sampleShadingEnable = false,
//         .alphaToCoverageEnable = false,
//         .alphaToOneEnable = false,
//     };
//     vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
//     vk::PipelineColorBlendStateCreateInfo color_blend{
//         .logicOpEnable = false,
//     };
//     vk::PipelineColorBlendAttachmentState color_blend_attachment{
//         .blendEnable = true,
//         .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
//         .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
//         .colorBlendOp = vk::BlendOp::eAdd,
//         .srcAlphaBlendFactor = vk::BlendFactor::eOne,
//         .dstAlphaBlendFactor = vk::BlendFactor::eZero,
//         .alphaBlendOp = vk::BlendOp::eAdd,
//         .colorWriteMask =
//             vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
//             vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
//     };
//     color_blend.setAttachments(color_blend_attachment);
//     vk::PipelineDynamicStateCreateInfo dynamic_state{};
//     auto viewport_dyn_states = {vk::DynamicState::eViewport,
//                                 vk::DynamicState::eScissor};
//     dynamic_state.setDynamicStates(viewport_dyn_states);
//
//     auto load_shader = [&](std::string_view path) {
//       std::ifstream file(path.data(), std::ios::binary);
//       std::vector<char> data((std::istreambuf_iterator<char>(file)),
//                              (std::istreambuf_iterator<char>()));
//       return vk::raii::ShaderModule{
//           device, vk::ShaderModuleCreateInfo{
//                       .codeSize = data.size(),
//                       .pCode = reinterpret_cast<vkvideo::u32 *>(data.data()),
//                   }};
//     };
//
//     auto vertex_shader = load_shader("build/app/fullscreen.vert.spv");
//     auto fragment_shader = load_shader("build/app/fullscreen.frag.spv");
//     vk::PipelineShaderStageCreateInfo shaders[2] = {
//         {
//             .stage = vk::ShaderStageFlagBits::eVertex,
//             .module = vertex_shader,
//             .pName = "main",
//         },
//         {
//             .stage = vk::ShaderStageFlagBits::eFragment,
//             .module = fragment_shader,
//             .pName = "main",
//         }};
//     vk::PipelineRenderingCreateInfo rendering_info{
//         .viewMask = 0,
//     };
//     rendering_info.setColorAttachmentFormats(color_attachment_format);
//
//     return vk::raii::Pipeline{device, nullptr,
//                               vk::GraphicsPipelineCreateInfo{
//                                   .pNext = &rendering_info,
//                                   .stageCount = std::size(shaders),
//                                   .pStages = shaders,
//                                   .pVertexInputState = &vertex_input,
//                                   .pInputAssemblyState = &input_assembly,
//                                   .pTessellationState = &tesselation,
//                                   .pViewportState = &viewport,
//                                   .pRasterizationState = &raster,
//                                   .pMultisampleState = &multisample,
//                                   .pDepthStencilState = &depth_stencil,
//                                   .pColorBlendState = &color_blend,
//                                   .pDynamicState = &dynamic_state,
//                                   .layout = pipeline_layout,
//                               }};
//   }
//
//   vk::raii::ImageView create_image_view(const vk::raii::Device &device,
//                                         const vkvideo::VideoFramePlane
//                                         &plane) {
//     vk::SamplerYcbcrConversionInfo conv_info{.conversion = *yuv_sampler};
//
//     return vk::raii::ImageView{
//         device, vk::ImageViewCreateInfo{
//                     .pNext = yuv_sampler != nullptr ? &conv_info : nullptr,
//                     .image = plane.image,
//                     .viewType = vk::ImageViewType::e2DArray,
//                     .format = plane.format,
//                     .components =
//                         vk::ComponentMapping{
//                             vk::ComponentSwizzle::eIdentity,
//                             vk::ComponentSwizzle::eIdentity,
//                             vk::ComponentSwizzle::eIdentity,
//                             vk::ComponentSwizzle::eIdentity,
//                         },
//                     .subresourceRange = plane.get_subresource_range(),
//                 }};
//   }
// };
//
// int main(int argc, char *argv[]) {
//   namespace vkv = vkvideo;
//   namespace vkr = vkv::vkr;
//   namespace vkff = vkv::ffmpeg;
//
//   if (argc < 2) {
//     std::cerr << "Usage: " << argv[0] << " <input.mkv>" << std::endl;
//     return 1;
//   }
//
//   vkff::Instance ffmpeg;
//
//   vkv::Context context{vkv::ContextArgs{
//       .mode = vkv::DisplayMode::Preview,
//       .width = 640,
//       .height = 360,
//       .fps = {30, 1},
//       .sample_rate = 44100,
//       .ch_layout = vkff::ch_layout_stereo,
//       .sample_format = AV_SAMPLE_FMT_S16,
//       .render_output = "output.mkv",
//   }};
//
//   auto video =
//       context.open_video(argv[1], {
//                                       // .hwaccel = vkv::HWAccel::eOff,
//                                       // .mode = vkv::DecodeMode::eReadAll,
//                                   });
//
//   auto &vk = context.get_vulkan();
//   vkr::CommandPool pool{
//       vk.get_device(),
//       vk::CommandPoolCreateInfo{
//           .flags = vk::CommandPoolCreateFlagBits::eTransient |
//                    vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
//           .queueFamilyIndex = static_cast<vkv::u32>(vk.get_qf_graphics()),
//       }};
//
//   auto fif_cnt = vk.get_swapchain_ctx().num_fifs();
//   vkr::CommandBuffers cmd_bufs{
//       vk.get_device(), vk::CommandBufferAllocateInfo{
//                            .commandPool = *pool,
//                            .level = vk::CommandBufferLevel::ePrimary,
//                            .commandBufferCount =
//                            static_cast<vkv::u32>(fif_cnt),
//                        }};
//   std::vector<vkv::TimelineSemaphore> cmd_buf_sems;
//   std::vector<std::vector<mcpp::unique_any>> cmd_buf_dependencies;
//   std::vector<vkr::Semaphore> present_sems;
//   cmd_buf_sems.reserve(fif_cnt);
//   cmd_buf_dependencies.resize(fif_cnt);
//   present_sems.reserve(fif_cnt);
//   for (vkv::i32 i = 0; i < fif_cnt; ++i) {
//     cmd_buf_sems.emplace_back(vk.get_device(), i);
//     present_sems.emplace_back(vk.get_device(), vk::SemaphoreCreateInfo{});
//   }
//
//   auto handle_transition = [&](vkv::VideoFrame &video_frame) {
//     auto &plane = video_frame.data->planes.front();
//
//     // handle queue ownership transfer...
//     if (plane.queue_family_idx != vk.get_qf_graphics() &&
//         plane.queue_family_idx != vk::QueueFamilyIgnored) {
//       auto cmd_buf = vk.get_temp_pools().begin(plane.queue_family_idx);
//       cmd_buf.begin(vk::CommandBufferBeginInfo{
//           .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
//       vk::ImageMemoryBarrier2 barrier{
//           .srcStageMask = plane.stage,
//           .srcAccessMask = plane.access,
//           .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
//           .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
//           .oldLayout = plane.layout,
//           .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
//           .srcQueueFamilyIndex = plane.queue_family_idx,
//           .dstQueueFamilyIndex = static_cast<vkv::u32>(vk.get_qf_graphics()),
//           .image = plane.image,
//           .subresourceRange = plane.get_subresource_range(),
//       };
//       cmd_buf.pipelineBarrier2(
//           vk::DependencyInfo{}.setImageMemoryBarriers(barrier));
//       cmd_buf.end();
//       vk::SemaphoreSubmitInfo wait_sem{
//           .semaphore = plane.semaphore,
//           .value = plane.semaphore_value,
//           .stageMask = plane.stage,
//       };
//
//       auto [rel_sem, rel_sem_value] = vk.get_temp_pools().end(
//           std::move(cmd_buf), plane.queue_family_idx, {}, wait_sem);
//
//       cmd_buf = vk.get_temp_pools().begin(vk.get_qf_graphics());
//       cmd_buf.begin(vk::CommandBufferBeginInfo{
//           .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
//       cmd_buf.pipelineBarrier2(
//           vk::DependencyInfo{}.setImageMemoryBarriers(barrier));
//       cmd_buf.end();
//
//       vk::SemaphoreSubmitInfo wait_sem2{
//           .semaphore = *rel_sem,
//           .value = rel_sem_value,
//           .stageMask = vk::PipelineStageFlagBits2::eBlit,
//       };
//       auto [acq_sem, acq_sem_value] = vk.get_temp_pools().end(
//           std::move(cmd_buf), vk.get_qf_graphics(), rel_sem, wait_sem2);
//
//       plane.semaphore = **acq_sem;
//       plane.semaphore_value = acq_sem_value;
//       plane.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
//       plane.queue_family_idx = vk.get_qf_graphics();
//       plane.stage = vk::PipelineStageFlagBits2::eFragmentShader;
//       plane.access = vk::AccessFlagBits2::eShaderSampledRead;
//       video_frame.data->add_buf(std::move(acq_sem));
//     } else if (plane.layout != vk::ImageLayout::eShaderReadOnlyOptimal) {
//       auto cmd_buf = vk.get_temp_pools().begin(vk.get_qf_graphics());
//       cmd_buf.begin(vk::CommandBufferBeginInfo{
//           .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
//       vk::ImageMemoryBarrier2 barrier{
//           .srcStageMask = plane.stage,
//           .srcAccessMask = plane.access,
//           .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
//           .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
//           .oldLayout = plane.layout,
//           .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
//           .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
//           .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
//           .image = plane.image,
//           .subresourceRange = {
//               .aspectMask = vk::ImageAspectFlagBits::eColor,
//               .levelCount = 1,
//               .layerCount = static_cast<vkv::u32>(plane.num_layers),
//           }};
//       cmd_buf.pipelineBarrier2(
//           vk::DependencyInfo{}.setImageMemoryBarriers(barrier));
//       cmd_buf.end();
//
//       vk::SemaphoreSubmitInfo wait_sem{
//           .semaphore = plane.semaphore,
//           .value = plane.semaphore_value,
//           .stageMask = plane.stage,
//       };
//       auto [trans_sem, trans_sem_value] = vk.get_temp_pools().end(
//           std::move(cmd_buf), vk.get_qf_graphics(), {}, wait_sem);
//
//       plane.semaphore = **trans_sem;
//       plane.semaphore_value = trans_sem_value;
//       plane.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
//       plane.stage = vk::PipelineStageFlagBits2::eFragmentShader;
//       plane.access = vk::AccessFlagBits2::eShaderSampledRead;
//       video_frame.data->add_buf(std::move(trans_sem));
//     }
//   };
//
//   auto start_time = std::chrono::high_resolution_clock::now();
//   auto prev_time = start_time;
//
//   std::unordered_map<VideoPipelineInfo, std::shared_ptr<VideoPipeline>>
//       pipelines;
//
//   for (vkv::i32 i = 0; context.alive(); ++i) {
//     context.update();
//
//     auto now = std::chrono::high_resolution_clock::now();
//     auto elapsed = now - prev_time;
//     prev_time = now;
//
//     auto elapsed_from_start = now - start_time;
//
//     auto &present = context.get_vulkan().get_swapchain_ctx();
//     auto frame = present.begin_frame();
//
//     cmd_buf_sems[frame.fif_idx].wait(frame.frame_idx, INT64_MAX);
//     // once work is done, we can free all dependencies
//     cmd_buf_dependencies[frame.fif_idx].clear();
//
//     if (auto image_opt = frame.acquire_image(UINT64_MAX);
//         image_opt.has_value()) {
//       auto [image_idx, image, image_view, image_size, image_format] =
//           image_opt.value();
//       // record cmdbuf
//       auto &cmd_buf = cmd_bufs[frame.fif_idx];
//       cmd_buf.begin(vk::CommandBufferBeginInfo{
//           .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
//
//       // transition: eUndefined -> eTransferDstOptimal
//       {
//         vk::ImageMemoryBarrier2 sc_img_trans{
//             .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
//             .srcAccessMask = vk::AccessFlagBits2::eNone,
//             .dstStageMask =
//             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
//             .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
//                              vk::AccessFlagBits2::eColorAttachmentRead,
//             .oldLayout = vk::ImageLayout::eUndefined,
//             .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
//             .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
//             .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
//             .image = image,
//             .subresourceRange = {
//                 .aspectMask = vk::ImageAspectFlagBits::eColor,
//                 .levelCount = 1,
//                 .layerCount = 1,
//             }};
//         cmd_buf.pipelineBarrier2(
//             vk::DependencyInfo{}.setImageMemoryBarriers(sc_img_trans));
//       }
//       auto video_frame = video->get_frame(elapsed_from_start.count() %
//                                           video->get_duration().value_or(1e9));
//       VideoPipelineInfo vid_info{
//           .color_attachment_format = image_format,
//           .pixel_format = video_frame.has_value() ? video_frame->frame_format
//                                                   : AV_PIX_FMT_NONE,
//       };
//       if (video_frame.has_value())
//         for (const auto &plane : video_frame->data->planes)
//           vid_info.plane_formats.push_back(plane.format);
//
//       auto it = pipelines.find(vid_info);
//       if (it == pipelines.end()) {
//         auto pipeline =
//             std::make_shared<VideoPipeline>(vk.get_device(), vid_info,
//             fif_cnt);
//         std::tie(it, std::ignore) = pipelines.try_emplace(vid_info,
//         pipeline);
//       }
//
//       auto pipeline = it->second;
//       vk::raii::ImageView view = nullptr;
//       if (video_frame.has_value())
//         view = pipeline->create_image_view(vk.get_device(),
//                                            video_frame->data->planes.front());
//
//       if (video_frame.has_value())
//         handle_transition(video_frame.value());
//
//       // here we use the huge ass graphics pipeline
//       vk::RenderingAttachmentInfo color_attachment{
//           .imageView = image_view,
//           .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
//           .loadOp = vk::AttachmentLoadOp::eClear,
//           .storeOp = vk::AttachmentStoreOp::eStore,
//           .clearValue = {vk::ClearColorValue{
//               .float32 = std::array<float, 4>{0.0f, 0.0f, 0.2f, 1.0f},
//           }},
//       };
//       cmd_buf.beginRendering(vk::RenderingInfo{
//           .renderArea = {{0, 0}, image_size},
//           .layerCount = 1,
//       }
//                                  .setColorAttachments(color_attachment));
//       cmd_buf.setViewport(0,
//                           vk::Viewport{
//                               .x = 0,
//                               .y = 0,
//                               .width = static_cast<float>(image_size.width),
//                               .height =
//                               static_cast<float>(image_size.height),
//                           });
//       cmd_buf.setScissor(0, vk::Rect2D{
//                                 .offset = {0, 0},
//                                 .extent = image_size,
//                             });
//       cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics,
//                            pipeline->pipeline);
//       cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
//                                  pipeline->pipeline_layout, 0,
//                                  *pipeline->descriptor_sets[frame.fif_idx],
//                                  {});
//       vk::DescriptorImageInfo desc_sampler{
//           .sampler = pipeline->sampler,
//           .imageView = view,
//           .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
//       };
//       vk.get_device().updateDescriptorSets(
//           vk::WriteDescriptorSet{
//               .dstSet = *pipeline->descriptor_sets[frame.fif_idx],
//               .dstBinding = 0,
//               .descriptorCount = 1,
//               .descriptorType = vk::DescriptorType::eCombinedImageSampler,
//               .pImageInfo = &desc_sampler,
//           },
//           {});
//       FrameInfoPushConstants pc_frame{};
//       if (video_frame.has_value()) {
//         pc_frame.frame_index =
//             static_cast<float>(video_frame->frame_index.value_or(0.0f));
//         pc_frame.uv_max[0] =
//             static_cast<float>(video_frame->data->width) /
//             static_cast<float>(video_frame->data->padded_width);
//         pc_frame.uv_max[1] =
//             static_cast<float>(video_frame->data->height) /
//             static_cast<float>(video_frame->data->padded_height);
//       }
//       cmd_buf.pushConstants<FrameInfoPushConstants>(
//           pipeline->pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
//           pc_frame);
//       cmd_buf.draw(3, 1, 0, 0);
//
//       cmd_buf.endRendering();
//
//       // transition: eTransferDstOptimal -> ePresentSrcKHR
//       {
//         vk::ImageMemoryBarrier2 sc_img_trans{
//             .srcStageMask =
//             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
//             .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
//                              vk::AccessFlagBits2::eColorAttachmentRead,
//             .dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
//             .dstAccessMask = vk::AccessFlagBits2::eNone,
//             .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
//             .newLayout = vk::ImageLayout::ePresentSrcKHR,
//             .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
//             .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
//             .image = image,
//             .subresourceRange = {
//                 .aspectMask = vk::ImageAspectFlagBits::eColor,
//                 .levelCount = 1,
//                 .layerCount = 1,
//             }};
//         cmd_buf.pipelineBarrier2(
//             vk::DependencyInfo{}.setImageMemoryBarriers(sc_img_trans));
//       }
//       cmd_buf.end();
//
//       {
//         vk::CommandBufferSubmitInfo cmd_buf_info{
//             .commandBuffer = cmd_buf,
//         };
//         std::vector<vk::SemaphoreSubmitInfo> wait_sem_info{
//             {
//                 .semaphore = frame.image_acq_sem,
//                 .stageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
//             },
//         };
//         if (video_frame.has_value()) {
//           auto &plane = video_frame->data->planes.front();
//           wait_sem_info.push_back({
//               .semaphore = plane.semaphore,
//               .value = plane.semaphore_value,
//               .stageMask = plane.stage,
//           });
//         }
//         vk::SemaphoreSubmitInfo sig_sem_info[2]{
//             {
//                 .semaphore = cmd_buf_sems[frame.fif_idx],
//                 .value = static_cast<vkv::u64>(frame.frame_idx + fif_cnt),
//                 .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
//             },
//             {
//                 .semaphore = present_sems[frame.fif_idx],
//                 .stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
//             },
//         };
//
//         vk.get_graphics_queue().submit2(
//             vk::SubmitInfo2{}
//                 .setCommandBufferInfos(cmd_buf_info)
//                 .setWaitSemaphoreInfos(wait_sem_info)
//                 .setSignalSemaphoreInfos(sig_sem_info));
//         cmd_buf_dependencies[frame.fif_idx].push_back(std::move(video_frame));
//         cmd_buf_dependencies[frame.fif_idx].push_back(std::move(view));
//         // for now the pipeline is cached indefinitely, but if we use some
//         // strategy like LRU caching, we must ensure that the pipeline live
//         at
//         // least as long as command buffer execution
//         cmd_buf_dependencies[frame.fif_idx].push_back(std::move(pipeline));
//       }
//
//       vk::Semaphore wait_sem = *present_sems[frame.fif_idx];
//       present.end_frame(image_idx, wait_sem);
//     }
//   }
//
//   context.get_vulkan().get_device().waitIdle();
//   return 0;
// }

import std;
import vkvideo;

int main() {
  namespace vkv = vkvideo;
  std::vector<vkv::u8> pixels;
  vkv::u32 width = 640, height = 360;
  for (vkv::u32 y = 0; y < height; ++y) {
    for (vkv::u32 x = 0; x < width; ++x) {
      vkv::u32 color =
          ((y & 0xff) << 16) | ((x & 0xff) << 8) | ((y + x) & 0xff);
      for (int ch = 0; ch < 4; ++ch) {
        pixels.push_back(color >> ((2 - ch) * 8));
      }
    }
  }

  vkv::medias::stbi::write_img(
      "test.png", width, height, pixels,
      vkv::medias::ffmpeg::PixelFormat::AV_PIX_FMT_RGBA);
  return 0;
}
