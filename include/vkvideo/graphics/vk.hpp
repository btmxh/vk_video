#pragma once

#include "codeccontext.h"
#include "stream.h"
#include "vkvideo/core/types.hpp"
#include "vkvideo/graphics/vma.hpp"
#include "vkvideo/medias/wrapper.hpp"
#include <mutex>
#include <vkfw/vkfw.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace vkvideo {
namespace vkr = vk::raii;

class QueueMutexMap {
public:
  void lock(u32 qfi, u32 qi);
  void unlock(u32 qfi, u32 qi);

  template <class T> T acquire(u32 qfi, u32 qi) {
    std::scoped_lock _lck{main_mutex};
    return T{get_mutex(_lck, qfi, qi)};
  }

private:
  std::mutex main_mutex;
  std::unordered_map<u64, std::unique_ptr<std::mutex>> mutexes;

  u64 calc_key(u32 qfi, u32 qi) { return (u64)qfi << 32 | (u64)qi; }

  std::mutex &get_mutex(std::scoped_lock<std::mutex> &main_lock, u32 qfi,
                        u32 qi);
};

class VkContext {
public:
  VkContext(vkfw::Window &window);

  VkContext(const VkContext &) = delete;
  VkContext &operator=(const VkContext &) = delete;

  VkContext(VkContext &&) = delete;
  VkContext &operator=(VkContext &&) = delete;

  template <class T, av::Direction dir>
  void enable_hardware_acceleration(av::VideoCodecContext<T, dir> &ctx) {
    ctx.raw()->hw_device_ctx = av_buffer_ref(hwdevice_ctx.get());
  }

  vk::raii::Instance &get_instance() { return instance; }
  vk::raii::Device &get_device() { return device; }
  vk::raii::PhysicalDevice &get_physical_device() { return physical_device; }
  VkMemAllocator &get_vma_allocator() { return allocator; }

  i32 get_qf_graphics() const { return qf_graphics; }
  i32 get_qf_compute() const { return qf_compute; }
  i32 get_qf_transfer() const { return qf_transfer; }

  template <class T = std::scoped_lock<std::mutex>>
  T lock_queue(u32 qfi, u32 qi) {
    return T{mutexes.lock(qfi, qi)};
  }

private:
  vkr::Context context;
  vkr::Instance instance = nullptr;
  vkr::SurfaceKHR surface = nullptr;
  vkr::DebugUtilsMessengerEXT debug_messenger = nullptr;
  vkr::PhysicalDevice physical_device = nullptr;
  vkr::Device device = nullptr;
  BufferRef hwdevice_ctx = nullptr;
  std::vector<const char *> device_extensions;
  vk::StructureChain<
      vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
      vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features,
      vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>
      feature_chain;
  QueueMutexMap mutexes;
  VkMemAllocator allocator = nullptr;

  i32 qf_graphics, qf_compute, qf_transfer;
};
} // namespace vkvideo
