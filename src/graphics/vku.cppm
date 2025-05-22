export module vkvideo.graphics:vku;

import std;
import vkvideo.core;
import vulkan_hpp;

export namespace vkvideo::graphics {
struct VulkanHandle {
  vk::ObjectType type;
  u64 handle;

  static auto ptr_to_u64(const void *ptr) {
    return static_cast<u64>(reinterpret_cast<std::uintmax_t>(ptr));
  }

  VulkanHandle(vk::Semaphore sem)
      : type{vk::ObjectType::eSemaphore}, handle{ptr_to_u64(sem)} {}
  VulkanHandle(vk::CommandBuffer cmd)
      : type{vk::ObjectType::eCommandBuffer}, handle{ptr_to_u64(cmd)} {}
  VulkanHandle(vk::Image img)
      : type{vk::ObjectType::eImage}, handle{ptr_to_u64(img)} {}
  VulkanHandle(vk::ImageView view)
      : type{vk::ObjectType::eImageView}, handle{ptr_to_u64(view)} {}
  VulkanHandle(vk::Queue queue)
      : type{vk::ObjectType::eQueue}, handle{ptr_to_u64(queue)} {}
};

void set_debug_label(vk::raii::Device &device, VulkanHandle handle,
                     const char *name) {
  device.setDebugUtilsObjectNameEXT({
      .objectType = handle.type,
      .objectHandle = handle.handle,
      .pObjectName = name,
  });
}
} // namespace vkvideo::graphics
