#pragma once

#include "vkvideo/core/types.hpp"
#include "vkvideo/graphics/mutex.hpp"
#include "vkvideo/graphics/swapchain.hpp"
#include "vkvideo/graphics/tx.hpp"
#include "vkvideo/graphics/vma.hpp"
#include "vkvideo/medias/ffmpeg.hpp"

#include <VkBootstrap.h>
#include <vkfw/vkfw.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_structs.hpp>

#include <mutex>

namespace vkvideo {
namespace vkr = vk::raii;

class VkContext {
public:
  VkContext(vkfw::Window &window);

  VkContext(const VkContext &) = delete;
  VkContext &operator=(const VkContext &) = delete;

  VkContext(VkContext &&) = delete;
  VkContext &operator=(VkContext &&) = delete;

  void enable_hardware_acceleration(ffmpeg::CodecContext &ctx);

  vk::raii::Instance &get_instance() { return instance; }
  vk::raii::Device &get_device() { return device; }
  vk::raii::PhysicalDevice &get_physical_device() { return physical_device; }
  VkMemAllocator &get_vma_allocator() { return allocator; }
  VkSwapchainContext &get_swapchain_ctx() { return swapchain_ctx; }

  i32 get_qf_graphics() const { return qf_graphics; }
  i32 get_qf_compute() const { return qf_compute; }
  i32 get_qf_transfer() const { return qf_transfer; }

  vkr::Queue &get_graphics_queue() { return q_graphics; }

  template <class T = std::scoped_lock<std::mutex>>
  T lock_queue(u32 qfi, u32 qi) {
    return T{mutexes.lock(qfi, qi)};
  }

  TempCommandPools &get_temp_pools() { return tx_pool; }

private:
  vkr::Context context;
  vkr::Instance instance = nullptr;
  vkr::SurfaceKHR surface = nullptr;
  vkr::DebugUtilsMessengerEXT debug_messenger = nullptr;
  vkr::PhysicalDevice physical_device = nullptr;
  vkr::Device device = nullptr;
  ffmpeg::BufferRef hwdevice_ctx = nullptr;
  std::vector<const char *> device_extensions;
  vk::StructureChain<
      vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
      vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features,
      vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>
      feature_chain;
  QueueMutexMap mutexes;
  VkMemAllocator allocator = nullptr;
  TempCommandPools tx_pool;

  VkSwapchainContext swapchain_ctx = nullptr;

  i32 qf_graphics, qf_compute, qf_transfer;
  vkr::Queue q_graphics = nullptr;
};
} // namespace vkvideo
