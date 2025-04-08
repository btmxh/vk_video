#include "vkvideo/graphics/vk.hpp"

#include "averror.h"
#include "vkfw/vkfw.hpp"
#include "vkvideo/graphics/vma.hpp"
#include "vkvideo/medias/wrapper.hpp"

#include <vkvideo/core/utility.hpp>
#include <vkvideo/version.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_handles.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_structs.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

namespace vkvideo {

inline VKAPI_ATTR VkBool32 VKAPI_CALL default_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData, void *) {
  switch (static_cast<u32>(pCallbackData->messageIdNumber)) {
  case 0x086974c1: /* BestPractices-vkCreateCommandPool-command-buffer-reset */
  case 0xfd92477a: /* BestPractices-vkAllocateMemory-small-allocation */
  case 0x618ab1e7: /* VUID-VkImageViewCreateInfo-usage-02275 */
  case 0x30f4ac70: /* VUID-VkImageCreateInfo-pNext-06811 */
  // https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/9680
  case 0x77c5e4e8:
  case 0x79b1d0c3:
  case 0xfd38b0b6:
    return VK_FALSE;
  default:
    break;
  }

  auto ms = vkb::to_string_message_severity(messageSeverity);
  auto mt = vkb::to_string_message_type(messageType);
  if (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
    printf("[%s: %s] %s - %s\n", ms, mt, pCallbackData->pMessageIdName,
           pCallbackData->pMessage);
  } else {
    printf("[%s: %s] %s\n", ms, mt, pCallbackData->pMessage);
  }

  return VK_FALSE; // Applications must return false here (Except Validation, if
                   // return true, will skip calling to driver)
}

std::mutex &QueueMutexMap::get_mutex(std::scoped_lock<std::mutex> &main_lock,
                                     u32 qfi, u32 qi) {
  if (!mutexes.contains(calc_key(qfi, qi))) {
    mutexes[calc_key(qfi, qi)] = std::make_unique<std::mutex>();
  }
  return *mutexes[calc_key(qfi, qi)];
}

VkContext::VkContext(vkfw::Window &window) {
  feature_chain.get<vk::PhysicalDeviceFeatures2>()
      .features.setVertexPipelineStoresAndAtomics(true)
      .setShaderInt64(true)
      .setFragmentStoresAndAtomics(true);
  feature_chain.get<vk::PhysicalDeviceVulkan11Features>()
      .setSamplerYcbcrConversion(true);
  feature_chain.get<vk::PhysicalDeviceVulkan12Features>()
      .setTimelineSemaphore(true)
      .setVulkanMemoryModel(true)
      .setVulkanMemoryModelDeviceScope(true)
      .setBufferDeviceAddress(true)
      .setUniformAndStorageBuffer8BitAccess(true);
  feature_chain.get<vk::PhysicalDeviceVulkan13Features>().setSynchronization2(
      true);
  feature_chain.get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>()
      .setVideoMaintenance1(true);
  auto b_inst =
      vkb::InstanceBuilder{}
          .request_validation_layers()
          // .add_validation_feature_enable(
          //     VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT)
          .add_validation_feature_enable(
              VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT)
          .add_validation_feature_enable(
              VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT)
          .add_validation_feature_enable(
              VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT)
          // .use_default_debug_messenger()
          .set_debug_callback(default_debug_callback)
          .add_debug_messenger_severity(
              VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
          .require_api_version(VK_API_VERSION_1_3)
          .set_app_name("vkvideo")
          .set_app_version(VK_MAKE_VERSION(VKVIDEO_VERSION_MAJOR,
                                           VKVIDEO_VERSION_MINOR,
                                           VKVIDEO_VERSION_PATCH))
          .set_engine_name("vkvideo")
          .set_engine_version(VK_MAKE_VERSION(VKVIDEO_VERSION_MAJOR,
                                              VKVIDEO_VERSION_MINOR,
                                              VKVIDEO_VERSION_PATCH));
  bool headless = vkfw::getPlatform() == vkfw::Platform::eNull;
  if (headless) {
    b_inst.set_headless();
  } else {
    auto extensions = vkfw::getRequiredInstanceExtensions();
    b_inst.enable_extensions(extensions.size(), extensions.data());
  }

  auto r_inst = b_inst.build();

  if (!r_inst.has_value()) {
    throw std::runtime_error{r_inst.error().message()};
  }

  auto &vkb_inst = r_inst.value();
  instance = vkr::Instance{context, vkb_inst.instance};
  debug_messenger =
      vkr::DebugUtilsMessengerEXT{instance, vkb_inst.debug_messenger};

  if (!headless) {
    surface =
        vkr::SurfaceKHR{instance, vkfw::createWindowSurface(*instance, window)};
  }

  vkb::PhysicalDeviceSelector selector{vkb_inst};
  auto r_phys_device =
      selector.set_minimum_version(1, 3).set_surface(*surface).select();

  if (!r_phys_device.has_value()) {
    throw std::runtime_error{r_phys_device.error().message()};
  }

  auto &vkb_phys_device = r_phys_device.value();
  device_extensions = std::vector<const char *>{
      vk::EXTExternalMemoryDmaBufExtensionName,
      vk::EXTImageDrmFormatModifierExtensionName,
      vk::KHRExternalMemoryFdExtensionName,
      vk::KHRExternalSemaphoreFdExtensionName,
      vk::EXTExternalMemoryHostExtensionName,
      vk::EXTDebugUtilsExtensionName,
#ifdef _WIN32
      vk::KHRExternalMemoryWin32ExtensionName,
      vk::KHRExternalSemaphoreWin32ExtensionName,
#endif
      vk::EXTDescriptorBufferExtensionName,
      vk::EXTPhysicalDeviceDrmExtensionName,
      vk::KHRVideoQueueExtensionName,
      vk::KHRVideoDecodeQueueExtensionName,
      vk::KHRVideoDecodeH264ExtensionName,
      vk::KHRVideoDecodeH265ExtensionName,
      vk::KHRVideoDecodeAv1ExtensionName,
      vk::EXTShaderAtomicFloatExtensionName,
      vk::KHRCooperativeMatrixExtensionName,
      vk::NVOpticalFlowExtensionName,
      vk::EXTShaderObjectExtensionName,
      vk::KHRPushDescriptorExtensionName,
      vk::KHRVideoMaintenance1ExtensionName,
      vk::KHRVideoEncodeQueueExtensionName,
      vk::KHRVideoEncodeH264ExtensionName,
      vk::KHRVideoEncodeH265ExtensionName,
  };

  std::erase_if(device_extensions, [&](const char *ext) {
    return !vkb_phys_device.enable_extension_if_present(ext);
  });

  physical_device = vkr::PhysicalDevice{instance, vkb_phys_device};
  vkb::DeviceBuilder b_device{r_phys_device.value()};
  b_device.add_pNext(&feature_chain.get<vk::PhysicalDeviceFeatures2>());
  auto r_device = b_device.build();

  if (!r_device.has_value()) {
    throw std::runtime_error{r_device.error().message()};
  }

  device = vkr::Device{physical_device, r_device.value()};

  auto hwdevice_ctx_ptr = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
  if (!hwdevice_ctx_ptr) {
    throw std::runtime_error{"Failed to allocate AVHWDeviceContext"};
  }

  hwdevice_ctx.reset(hwdevice_ctx_ptr);
  auto &hwdevice_ctx_data =
      *reinterpret_cast<AVHWDeviceContext *>(hwdevice_ctx->data);
  auto &vk_device_ctx =
      *reinterpret_cast<AVVulkanDeviceContext *>(hwdevice_ctx_data.hwctx);

  vk_device_ctx.inst = *instance;
  vk_device_ctx.get_proc_addr = vkGetInstanceProcAddr;
  vk_device_ctx.phys_dev = *physical_device;
  vk_device_ctx.act_dev = *device;
  vk_device_ctx.nb_enabled_inst_extensions = 0; // currently this is not used
  vk_device_ctx.enabled_inst_extensions = nullptr;
  // auto features = feature_chain.get<vk::PhysicalDeviceFeatures2>();
  auto features = feature_chain.get<vk::PhysicalDeviceFeatures2>();
  vk_device_ctx.device_features =
      *reinterpret_cast<VkPhysicalDeviceFeatures2 *>(&features);

  vk_device_ctx.nb_enabled_dev_extensions = device_extensions.size();
  vk_device_ctx.enabled_dev_extensions = device_extensions.data();

  hwdevice_ctx_data.user_opaque = this;
  vk_device_ctx.lock_queue = [](AVHWDeviceContext *ctx, u32 qfi, u32 qi) {
    static_cast<VkContext *>(ctx->user_opaque)->mutexes.lock(qfi, qi);
  };
  vk_device_ctx.unlock_queue = [](AVHWDeviceContext *ctx, u32 qfi, u32 qi) {
    static_cast<VkContext *>(ctx->user_opaque)->mutexes.unlock(qfi, qi);
  };

  auto qf_props = physical_device.getQueueFamilyProperties2<vk::StructureChain<
      vk::QueueFamilyProperties2, vk::QueueFamilyVideoPropertiesKHR>>();
  for (std::size_t i = 0;
       i < std::min(std::size(vk_device_ctx.qf), qf_props.size()); ++i) {
    const auto &[prop, video_prop] = qf_props[i];
    auto &qf = vk_device_ctx.qf[i++];
    qf.idx = i;
    qf.num = 0;
    qf.flags = static_cast<VkQueueFlagBits>(
        static_cast<u32>(prop.queueFamilyProperties.queueFlags));
    qf.video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(
        static_cast<u32>(video_prop.videoCodecOperations));
  }

  auto set_qf = [&](int &index, int &num, i32 value) {
    index = value;
    num = 1;
  };

  set_qf(vk_device_ctx.queue_family_index, vk_device_ctx.nb_graphics_queues,
         r_device.value().get_queue_index(vkb::QueueType::graphics).value());
  set_qf(vk_device_ctx.queue_family_comp_index, vk_device_ctx.nb_comp_queues,
         r_device.value().get_queue_index(vkb::QueueType::compute).value());
  set_qf(vk_device_ctx.queue_family_tx_index, vk_device_ctx.nb_tx_queues,
         r_device.value().get_queue_index(vkb::QueueType::transfer).value());
  auto it =
      std::find_if(qf_props.begin(), qf_props.end(), [](const auto &prop) {
        return prop.template get<vk::QueueFamilyProperties2>()
                   .queueFamilyProperties.queueFlags &
               vk::QueueFlagBits::eVideoEncodeKHR;
      });
  set_qf(vk_device_ctx.queue_family_encode_index,
         vk_device_ctx.nb_encode_queues, it - qf_props.begin());
  it = std::find_if(qf_props.begin(), qf_props.end(), [](auto &prop) {
    return prop.template get<vk::QueueFamilyProperties2>()
               .queueFamilyProperties.queueFlags &
           vk::QueueFlagBits::eVideoDecodeKHR;
  });
  set_qf(vk_device_ctx.queue_family_decode_index,
         vk_device_ctx.nb_decode_queues, it - qf_props.begin());
  vk_device_ctx.memory_alloc_cb = [](AVHWDeviceContext *ctx,
                                     VkMemoryRequirements *req,
                                     VkMemoryPropertyFlagBits req_flags,
                                     void *alloc_extension,
                                     VkMemoryPropertyFlagBits *mem_flags,
                                     AVVulkanDeviceMemory *out_mem) {
    bool dedicated_alloc = false;
    if (alloc_extension) {
      for (auto pNext = static_cast<const VkBaseInStructure *>(alloc_extension);
           pNext;
           pNext = static_cast<const VkBaseInStructure *>(pNext->pNext)) {
        if (static_cast<VkBaseInStructure *>(alloc_extension)->sType ==
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
          dedicated_alloc = true;
        }
      }

      // TODO: use memory pool to allocate memory for other pNext values
    }

    VmaAllocation allocation;
    VmaAllocationInfo allocation_info;
    VmaAllocationCreateInfo alloc_info = {
        .flags = dedicated_alloc ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
                                 : static_cast<VmaAllocationCreateFlags>(0),
        .requiredFlags = static_cast<VkMemoryPropertyFlags>(
            static_cast<u32>(req_flags) == UINT32_MAX ? 0 : req_flags),
    };
    auto &allocator = static_cast<VkContext *>(ctx->user_opaque)->allocator;
    auto ret =
        allocator.allocate(*reinterpret_cast<vk::MemoryRequirements *>(req),
                           alloc_info, allocation, allocation_info);
    if (ret != vk::Result::eSuccess) {
      auto msg = vk::to_string(ret);
      av_log(ctx, AV_LOG_ERROR, "Failed to allocate memory: %s\n", msg.c_str());
      return AVERROR(ENOMEM);
    }

    out_mem->memory = allocation_info.deviceMemory;
    out_mem->offset = allocation_info.offset;
    if (mem_flags)
      vmaGetAllocationMemoryProperties(
          *allocator, allocation,
          reinterpret_cast<VkMemoryPropertyFlags *>(mem_flags));
    out_mem->user = allocation;

    return 0;
  };

  vk_device_ctx.memory_free_cb = [](AVHWDeviceContext *ctx,
                                    const AVVulkanDeviceMemory *mem) {
    auto &allocator = static_cast<VkContext *>(ctx->user_opaque)->allocator;
    allocator.free(static_cast<VmaAllocation>(mem->user));
  };

  vk_device_ctx.memory_map_cb = [](AVHWDeviceContext *ctx,
                                   const AVVulkanDeviceMemory *mem,
                                   std::size_t size, void **ptr) {
    auto &allocator = static_cast<VkContext *>(ctx->user_opaque)->allocator;
    auto ret = allocator.map_memory(static_cast<VmaAllocation>(mem->user), ptr);
    if (ret != vk::Result::eSuccess) {
      auto msg = vk::to_string(ret);
      av_log(ctx, AV_LOG_ERROR, "Failed to map memory: %s\n", msg.c_str());
      return AVERROR_EXTERNAL;
    }
    return 0;
  };

  vk_device_ctx.memory_unmap_cb = [](AVHWDeviceContext *ctx,
                                     const AVVulkanDeviceMemory *mem) {
    auto &allocator = static_cast<VkContext *>(ctx->user_opaque)->allocator;
    allocator.unmap_memory(static_cast<VmaAllocation>(mem->user));
  };

  int err = av_hwdevice_ctx_init(hwdevice_ctx.get());
  if (err < 0)
    av::throws_if(av::OptionalErrorCode::null(), err, av::ffmpeg_category());

  allocator = VkMemAllocator::create(instance, physical_device, device);
  qf_graphics = vk_device_ctx.queue_family_index;
  qf_compute = vk_device_ctx.queue_family_comp_index;
  qf_transfer = vk_device_ctx.queue_family_tx_index;

  q_graphics = device.getQueue(qf_graphics, 0);

  window.callbacks()->on_framebuffer_resize = [&](vkfw::Window w, size_t width,
                                                  size_t height) {
    if (width > 0 && height > 0)
      swapchain_ctx.recreate(static_cast<i32>(width), static_cast<i32>(height));
  };

  tx_pool.init(device, mutexes);
  swapchain_ctx = VkSwapchainContext{
      device, r_device.value(), window, surface, q_graphics, 3,
  };
}

void VkContext::enable_hardware_acceleration(ffmpeg::CodecContext &ctx) {
  ctx->hw_device_ctx = av_buffer_ref(hwdevice_ctx.get());
}
} // namespace vkvideo
