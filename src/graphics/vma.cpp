#define VMA_IMPLEMENTATION
#include "vkvideo/graphics/vma.hpp"

#include <vulkan/vulkan_raii.hpp>

namespace vkvideo {

void VmaMappedBuffer::Deleter::operator()(VmaBuffer *buffer) {
  vmaUnmapMemory(buffer->allocator, buffer->get_allocation());
}

vk::ResultValueType<VmaMappedBuffer>::type VmaBuffer::map_memory() {
  void *ptr;
  vk::Result result{vmaMapMemory(allocator, allocation, &ptr)};
  vk::detail::resultCheck(result, "vma::mapMemory");
  auto size = static_cast<std::size_t>(alloc_info.size);
  return vk::detail::createResultValueType(
      result,
      VmaMappedBuffer{this, std::span<u8>{static_cast<u8 *>(ptr), size}});
}

vk::ResultValueType<VkMemAllocator>::type
VkMemAllocator::create(vk::raii::Instance &inst,
                       vk::raii::PhysicalDevice &phys_dev,
                       vk::raii::Device &dev) {
  VmaAllocator allocator;
  VmaVulkanFunctions funcs = VmaVulkanFunctions{
      .vkGetInstanceProcAddr = inst.getDispatcher()->vkGetInstanceProcAddr,
      .vkGetDeviceProcAddr = dev.getDispatcher()->vkGetDeviceProcAddr,
  };
  VmaAllocatorCreateInfo create_info{
      .physicalDevice = *phys_dev,
      .device = *dev,
      .pVulkanFunctions = &funcs,
      .instance = *inst,
      .vulkanApiVersion = VK_API_VERSION_1_3,
  };

  auto result = vk::Result{vmaCreateAllocator(&create_info, &allocator)};
  vk::detail::resultCheck(result, "vma::createAllocator");
  return vk::detail::createResultValueType(result, VkMemAllocator{allocator});
}

VkMemAllocator::VkMemAllocator(VmaAllocator allocator) : allocator{allocator} {}

VkMemAllocator::~VkMemAllocator() {
  if (allocator)
    vmaDestroyAllocator(allocator);
}

vk::ResultValueType<VmaBuffer>::type
VkMemAllocator::createBuffer(const vk::BufferCreateInfo &create_info,
                             const VmaAllocationCreateInfo &alloc_info) {
  VkBuffer buffer;
  VmaAllocation allocation;
  VmaAllocationInfo allocation_info;
  vk::Result result{vmaCreateBuffer(
      allocator, reinterpret_cast<const VkBufferCreateInfo *>(&create_info),
      &alloc_info, &buffer, &allocation, &allocation_info)};
  vk::detail::resultCheck(result, "vma::createBuffer");
  return vk::detail::createResultValueType(
      result, VmaBuffer{allocator, buffer, allocation, allocation_info});
}

} // namespace vkvideo
