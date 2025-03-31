#include "vkvideo/graphics/vk.hpp"

#include "averror.h"
#include "vkfw/vkfw.hpp"
#include "vkvideo/graphics/vma.hpp"
#include "vkvideo/medias/wrapper.hpp"
#include <VkBootstrap.h>
#include <vkvideo/core/utility.hpp>
#include <vkvideo/version.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_handles.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_structs.hpp>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

namespace vkvideo {

void QueueMutexMap::lock(u32 qfi, u32 qi) {
  std::scoped_lock _lck{main_mutex};
  get_mutex(_lck, qfi, qi).lock();
}

void QueueMutexMap::unlock(u32 qfi, u32 qi) {
  std::scoped_lock _lck{main_mutex};
  get_mutex(_lck, qfi, qi).unlock();
}

std::mutex &QueueMutexMap::get_mutex(std::scoped_lock<std::mutex> &main_lock,
                                     u32 qfi, u32 qi) {
  if (!mutexes.contains(calc_key(qfi, qi))) {
    mutexes[calc_key(qfi, qi)] = std::make_unique<std::mutex>();
  }
  return *mutexes[calc_key(qfi, qi)];
}

VkContext::VkContext(vkfw::Window &window) {
  feature_chain.get<vk::PhysicalDeviceVulkan11Features>()
      .setSamplerYcbcrConversion(true);
  feature_chain.get<vk::PhysicalDeviceVulkan12Features>().setTimelineSemaphore(
      true);
  feature_chain.get<vk::PhysicalDeviceVulkan13Features>().setSynchronization2(
      true);
  feature_chain.get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>()
      .setVideoMaintenance1(true);
  auto b_inst = vkb::InstanceBuilder{}
                    // .request_validation_layers()
                    // .use_default_debug_messenger()
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
      selector.set_minimum_version(1, 4).set_surface(*surface).select();

  if (!r_phys_device.has_value()) {
    throw std::runtime_error{r_phys_device.error().message()};
  }

  auto &vkb_phys_device = r_phys_device.value();
  device_extensions = std::vector<const char *>{
      vk::EXTExternalMemoryDmaBufExtensionName,
      vk::EXTImageDrmFormatModifierExtensionName,
      vk::KHRBindMemory2ExtensionName,
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
      *reinterpret_cast<AVHWDeviceContext *>(hwdevice_ctx.data().data());
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

  int err = av_hwdevice_ctx_init(hwdevice_ctx.get());
  if (err < 0)
    av::throws_if(av::OptionalErrorCode::null(), err, av::ffmpeg_category());

  allocator = VkMemAllocator::create(instance, physical_device, device);
}
} // namespace vkvideo
