module;

#include <vkvideo/version.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_hpp_macros.hpp>

export module vkvideo.graphics:vkc;

import std;
import vulkan_hpp;
import vkfw;
import vk_mem_alloc_hpp;
import vkvideo.core;
import vkvideo.third_party;
import :queues;
import :temppools;
import :swapchain;
import :vku;

export namespace vkvideo::graphics {

inline VKAPI_ATTR vk::Bool32 VKAPI_CALL default_debug_callback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    vk::DebugUtilsMessageTypeFlagsEXT messageType,
    const vk::DebugUtilsMessengerCallbackDataEXT *pCallbackData, void *) {
  switch (static_cast<u32>(pCallbackData->messageIdNumber)) {
  case 0x086974c1: /* BestPractices-vkCreateCommandPool-command-buffer-reset */
  case 0xfd92477a: /* BestPractices-vkAllocateMemory-small-allocation */
  case 0x618ab1e7: /* VUID-VkImageViewCreateInfo-usage-02275 */
  case 0x30f4ac70: /* VUID-VkImageCreateInfo-pNext-06811 */
  // https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/9680
  case 0x77c5e4e8:
  case 0x79b1d0c3:
  case 0xfd38b0b6:
    return false;
  default:
    break;
  }

  auto ms = vk::to_string(messageSeverity);
  auto mt = vk::to_string(messageType);
  if (messageSeverity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
    int x = 10;
  }
  // TODO: handle ANSI colors properly
  if (messageType & vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation) {
    std::println("\033[31m[{}: {}]\033[0m {} (ID {:x}) - {}", ms, mt,
                 pCallbackData->pMessageIdName, pCallbackData->messageIdNumber,
                 pCallbackData->pMessage);
  } else {
    std::println("\033[31m[{}: {}]\033[0m {}", ms, mt, pCallbackData->pMessage);
  }

  return false; // Applications must return false here (Except Validation, if
                // return true, will skip calling to driver)
}

class VkContext {
public:
  VkContext(vkfw::Window &window) {
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
    feature_chain.get<vk::PhysicalDeviceVulkan13Features>()
        .setSynchronization2(true)
        .setDynamicRendering(true);
    feature_chain.get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>()
        .setVideoMaintenance1(true);

    vk::ApplicationInfo app_info{
        .pApplicationName = "vkvideo",
        .applicationVersion =
            VK_MAKE_VERSION(VKVIDEO_VERSION_MAJOR, VKVIDEO_VERSION_MINOR,
                            VKVIDEO_VERSION_PATCH),
        .pEngineName = "vkvideo",
        .engineVersion =
            VK_MAKE_VERSION(VKVIDEO_VERSION_MAJOR, VKVIDEO_VERSION_MINOR,
                            VKVIDEO_VERSION_PATCH),
        .apiVersion = vk::ApiVersion13,
    };

    vk::DebugUtilsMessengerCreateInfoEXT debug_msg_ci{
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        .pfnUserCallback = default_debug_callback,
    };

    static constexpr bool enable_validation = true;

    auto window_inst_exts = vkfw::getRequiredInstanceExtensions();
    std::vector<const char *> inst_exts;

    bool headless = vkfw::getPlatform() == vkfw::Platform::eNull;
    if (!headless)
      inst_exts.insert(inst_exts.end(), window_inst_exts.begin(),
                       window_inst_exts.end());

    vk::InstanceCreateInfo inst_info{
        .pApplicationInfo = &app_info,
    };

    auto supported_extensions = context.enumerateInstanceExtensionProperties();

    std::vector<std::string> layer_storage;
    std::vector<const char *> layer_names;

    auto add_layer = [&](auto &&...args) {
      const auto &name =
          layer_storage.emplace_back(std::forward<decltype(args)>(args)...);
      layer_names.push_back(name.c_str());
    };

    const char *layers = std::getenv("VKVIDEO_ADDITIONAL_LAYERS");
    if (!layers)
      layers = "";

    for (const auto &layer :
         std::views::split(std::string_view{layers}, std::string_view{":"}))
      add_layer(std::string_view{layer});

    if constexpr (enable_validation) {
      add_layer("VK_LAYER_KHRONOS_validation");
      inst_exts.push_back(vk::EXTDebugUtilsExtensionName);
      inst_exts.push_back(vk::EXTLayerSettingsExtensionName);
    }

    inst_info.setPEnabledLayerNames(layer_names);

    std::erase_if(inst_exts, [&](const auto &ext_name) {
      return std::ranges::find_if(supported_extensions, [&](const auto &ext) {
               return strcmp(ext.extensionName.data(), ext_name) == 0;
             }) == supported_extensions.end();
    });

    inst_info.setPEnabledExtensionNames(inst_exts);
    instance = vk::raii::Instance{context, inst_info};
    if (std::ranges::find(inst_exts,
                          std::string_view{vk::EXTDebugUtilsExtensionName}) !=
        inst_exts.end())
      debug_messenger =
          vk::raii::DebugUtilsMessengerEXT{instance, debug_msg_ci};

    if (!headless) {
      surface = vk::raii::SurfaceKHR{
          instance, vkfw::createWindowSurface(*instance, window)};
    }

    vk::raii::PhysicalDevices physical_devices{instance};
    if (physical_devices.empty()) {
      throw std::runtime_error{"No physical devices (GPUs) found."};
    }

    auto pd_it = std::find_if(physical_devices.begin(), physical_devices.end(),
                              [](const vk::raii::PhysicalDevice &pd) {
                                return pd.getProperties().deviceType ==
                                       vk::PhysicalDeviceType::eDiscreteGpu;
                              });
    if (pd_it == physical_devices.end()) {
      pd_it = physical_devices.begin();
    }

    physical_device = std::move(*pd_it);
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
    if (!headless)
      device_extensions.push_back(vk::KHRSwapchainExtensionName);

    auto pd_ext_props = physical_device.enumerateDeviceExtensionProperties();
    std::set<std::string_view> pd_exts;
    for (const auto &prop : pd_ext_props) {
      pd_exts.insert(prop.extensionName.data());
    }

    std::erase_if(device_extensions, [&](const char *ext) {
      return !pd_exts.contains(std::string_view{ext});
    });

    // we use one graphics queue (which should supports present),
    // one compute queue (preferably async),
    // one transfer queue (preferably dedicated),
    // and as many video decode and encode queues as possible
    using QFPChain = vk::StructureChain<vk::QueueFamilyProperties2,
                                        vk::QueueFamilyVideoPropertiesKHR>;
    auto qf_props = physical_device.getQueueFamilyProperties2<QFPChain>();
    auto qf_graphics =
        std::find_if(qf_props.begin(), qf_props.end(),
                     [](const QFPChain &qf) {
                       return qf.get<vk::QueueFamilyProperties2>()
                                  .queueFamilyProperties.queueFlags &
                              vk::QueueFlagBits::eGraphics;
                     }) -
        qf_props.begin();
    if (qf_graphics == qf_props.size())
      throw std::runtime_error{"Graphics queue not found"};

    auto qf_compute =
        std::find_if(qf_props.begin(), qf_props.end(),
                     [&](const QFPChain &qf) {
                       auto idx = &qf - &qf_props[0];
                       return idx != qf_graphics &&
                              (qf.get<vk::QueueFamilyProperties2>()
                                   .queueFamilyProperties.queueFlags &
                               vk::QueueFlagBits::eCompute);
                     }) -
        qf_props.begin();
    // From the Vulkan spec
    // (https://registry.khronos.org/vulkan/specs/latest/man/html/VkQueueFlagBits.html)
    //
    // If an implementation exposes any queue family that supports graphics
    // operations, at least one queue family of at least one physical device
    // exposed by the implementation must support both graphics and compute
    // operations.
    //
    // Since the above find_if fails, the queue family pointed by qf_graphics
    // must also support compute operations.
    if (qf_compute == qf_props.size())
      qf_compute = qf_graphics;

    // From the Vulkan spec
    // (https://registry.khronos.org/vulkan/specs/latest/man/html/VkQueueFlagBits.html)
    //
    // All commands that are allowed on a queue that supports transfer
    // operations are also allowed on a queue that supports either graphics or
    // compute operations. Thus, if the capabilities of a queue family include
    // VK_QUEUE_GRAPHICS_BIT or VK_QUEUE_COMPUTE_BIT, then reporting the
    // VK_QUEUE_TRANSFER_BIT capability separately for that queue family is
    // optional.

    auto qf_transfer =
        std::find_if(qf_props.begin(), qf_props.end(),
                     [&](const QFPChain &qf) {
                       auto idx = &qf - &qf_props[0];
                       return idx != qf_graphics && idx != qf_compute &&
                              (qf.get<vk::QueueFamilyProperties2>()
                                   .queueFamilyProperties.queueFlags &
                               vk::QueueFlagBits::eTransfer);
                     }) -
        qf_props.begin();
    if (qf_transfer == qf_props.size())
      qf_transfer = qf_graphics;

    std::vector<u32> video_qf_indices;
    for (u32 i = 0; i < qf_props.size(); ++i) {
      if (qf_props[i]
              .get<vk::QueueFamilyVideoPropertiesKHR>()
              .videoCodecOperations) {
        video_qf_indices.push_back(i);
      }
    }

    std::vector<vk::DeviceQueueCreateInfo> queue_infos;
    float priority = 1.0;

    auto hwdevice_ctx_ptr = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!hwdevice_ctx_ptr) {
      throw std::runtime_error{"Failed to allocate AVHWDeviceContext"};
    }

    hwdevice_ctx.reset(hwdevice_ctx_ptr);
    auto &hwdevice_ctx_data =
        *reinterpret_cast<AVHWDeviceContext *>(hwdevice_ctx->data);
    auto &vk_device_ctx =
        *reinterpret_cast<AVVulkanDeviceContext *>(hwdevice_ctx_data.hwctx);

    auto add_queue_info = [&](u32 index) {
      queue_infos.push_back(vk::DeviceQueueCreateInfo{
          .queueFamilyIndex = index,
          .queueCount = 1,
          .pQueuePriorities = &priority,
      });
      vk_device_ctx.qf[vk_device_ctx.nb_qf++] = AVVulkanDeviceQueueFamily{
          .idx = static_cast<int>(index),
          .num = 1,
          .flags = static_cast<VkQueueFlagBits>(
              static_cast<u32>(qf_props[index]
                                   .get<vk::QueueFamilyProperties2>()
                                   .queueFamilyProperties.queueFlags)),
          .video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(
              static_cast<u32>(qf_props[index]
                                   .get<vk::QueueFamilyVideoPropertiesKHR>()
                                   .videoCodecOperations)),
      };
    };

    add_queue_info(qf_graphics);
    add_queue_info(qf_compute);
    add_queue_info(qf_transfer);
    for (auto qfi : video_qf_indices)
      add_queue_info(qfi);

    device = vk::raii::Device{
        physical_device,
        vk::DeviceCreateInfo{}
            .setPNext(&feature_chain.get<vk::PhysicalDeviceFeatures2>())
            .setQueueCreateInfos(queue_infos)
            .setPEnabledExtensionNames(device_extensions)};

    vk_device_ctx.inst = *instance;
    vk_device_ctx.get_proc_addr =
        instance.getDispatcher()->vkGetInstanceProcAddr;
    vk_device_ctx.phys_dev = *physical_device;
    vk_device_ctx.act_dev = *device;
    vk_device_ctx.nb_enabled_inst_extensions = 0; // currently this is not used
    vk_device_ctx.enabled_inst_extensions = nullptr;
    vk_device_ctx.device_features =
        *reinterpret_cast<VkPhysicalDeviceFeatures2 *>(
            &feature_chain.get<vk::PhysicalDeviceFeatures2>());
    vk_device_ctx.nb_enabled_dev_extensions = device_extensions.size();
    vk_device_ctx.enabled_dev_extensions = device_extensions.data();

    hwdevice_ctx_data.user_opaque = this;
    vk_device_ctx.lock_queue = [](AVHWDeviceContext *ctx, u32 qfi, u32 qi) {
      static_cast<VkContext *>(ctx->user_opaque)
          ->queues.get_mutexes()
          .lock(qfi, qi);
    };
    vk_device_ctx.unlock_queue = [](AVHWDeviceContext *ctx, u32 qfi, u32 qi) {
      static_cast<VkContext *>(ctx->user_opaque)
          ->queues.get_mutexes()
          .unlock(qfi, qi);
    };

    auto set_qf = [&](int &index, int &num, i32 value) {
      index = value;
      num = 1;
    };

    vk_device_ctx.queue_family_index = qf_graphics;
    vk_device_ctx.queue_family_comp_index = qf_compute;
    vk_device_ctx.queue_family_tx_index = qf_transfer;
    auto it2 = std::find_if(
        video_qf_indices.begin(), video_qf_indices.end(), [&](u32 i) {
          return qf_props[i]
                     .template get<vk::QueueFamilyProperties2>()
                     .queueFamilyProperties.queueFlags &
                 vk::QueueFlagBits::eVideoEncodeKHR;
        });
    vk_device_ctx.queue_family_encode_index =
        it2 == video_qf_indices.end() ? -1 : *it2;
    it2 = std::find_if(video_qf_indices.begin(), video_qf_indices.end(),
                       [&](u32 i) {
                         return qf_props[i]
                                    .template get<vk::QueueFamilyProperties2>()
                                    .queueFamilyProperties.queueFlags &
                                vk::QueueFlagBits::eVideoDecodeKHR;
                       });
    vk_device_ctx.queue_family_decode_index =
        it2 == video_qf_indices.end() ? -1 : *it2;

#ifdef VKVIDEO_USE_FFMPEG_CUSTOM_ALLOC
    // https://github.com/btmxh/FFmpeg/tree/n7.1-vma
    vk_device_ctx.memory_alloc_cb = [](AVHWDeviceContext *ctx,
                                       VkMemoryRequirements *req,
                                       VkMemoryPropertyFlagBits req_flags,
                                       void *alloc_extension,
                                       VkMemoryPropertyFlagBits *mem_flags,
                                       AVVulkanDeviceMemory *out_mem) {
      bool dedicated_alloc = false;
      if (alloc_extension) {
        for (auto pNext =
                 static_cast<const VkBaseInStructure *>(alloc_extension);
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
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate memory: %s\n",
               msg.c_str());
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
      auto ret =
          allocator.map_memory(static_cast<VmaAllocation>(mem->user), ptr);
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
#endif

    tp::ffmpeg::av_call(av_hwdevice_ctx_init(hwdevice_ctx.get()));
    auto vkfuncs = vma::functionsFromDispatcher(instance.getDispatcher(),
                                                device.getDispatcher());
    allocator = vma::createAllocatorUnique(vma::AllocatorCreateInfo{
        .physicalDevice = *physical_device,
        .device = *device,
        .pVulkanFunctions = &vkfuncs,
        .instance = *instance,
        .vulkanApiVersion = vk::ApiVersion13,
    });

    qf_graphics = vk_device_ctx.queue_family_index;
    qf_compute = vk_device_ctx.queue_family_comp_index;
    qf_transfer = vk_device_ctx.queue_family_tx_index;
    queues.init(device, qf_graphics, qf_compute, qf_transfer,
                std::move(video_qf_indices));

    if (!headless) {
      swapchain_ctx.emplace(physical_device, device, window, surface, 3);

      window.callbacks()->on_framebuffer_resize =
          [&](vkfw::Window w, size_t width, size_t height) {
            if (width > 0 && height > 0 && swapchain_ctx.has_value())
              swapchain_ctx->recreate(static_cast<i32>(width),
                                      static_cast<i32>(height));
          };
    }

    tx_pool.init(device, queues);
  }

  VkContext(const VkContext &) = delete;
  VkContext &operator=(const VkContext &) = delete;

  VkContext(VkContext &&) = delete;
  VkContext &operator=(VkContext &&) = delete;

  void enable_hardware_acceleration(tp::ffmpeg::CodecContext &ctx);

  vk::raii::Instance &get_instance() { return instance; }
  vk::raii::Device &get_device() { return device; }
  vk::raii::PhysicalDevice &get_physical_device() { return physical_device; }
  vma::Allocator &get_vma_allocator() { return *allocator; }

  const tp::ffmpeg::BufferRef &get_hwaccel_ctx() const { return hwdevice_ctx; }
  QueueManager &get_queues() { return queues; }
  VkSwapchainContext *get_swapchain_ctx() {
    return swapchain_ctx.has_value() ? &*swapchain_ctx : nullptr;
  }
  TempCommandPools &get_temp_pools() { return tx_pool; }

  void set_debug_label(VulkanHandle handle, const char *name) {
    ::vkvideo::graphics::set_debug_label(device, handle, name);
  }

private:
  vk::raii::Context context;
  vk::raii::Instance instance = nullptr;
  vk::raii::SurfaceKHR surface = nullptr;
  vk::raii::DebugUtilsMessengerEXT debug_messenger = nullptr;
  vk::raii::PhysicalDevice physical_device = nullptr;
  vk::raii::Device device = nullptr;
  tp::ffmpeg::BufferRef hwdevice_ctx = nullptr;
  std::vector<const char *> device_extensions;
  vk::StructureChain<
      vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
      vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features,
      vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>
      feature_chain;
  vma::UniqueAllocator allocator{nullptr};

  QueueManager queues;
  TempCommandPools tx_pool;
  std::optional<VkSwapchainContext> swapchain_ctx;
};

} // namespace vkvideo::graphics
