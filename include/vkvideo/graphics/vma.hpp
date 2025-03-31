#pragma once

#include "vkvideo/core/types.hpp"
#include <span>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>

namespace vkvideo {

class VmaBuffer;

class VmaMappedBuffer {
public:
  std::span<u8> data() { return data_span; }

private:
  friend class VmaBuffer;
  VmaMappedBuffer(VmaBuffer *buffer, std::span<u8> data_span)
      : buffer{buffer}, data_span{data_span} {};

  struct Deleter {
    void operator()(VmaBuffer *buffer);
  };

  std::unique_ptr<VmaBuffer, Deleter> buffer;
  std::span<u8> data_span;
};

class VmaBuffer {
public:
  VmaBuffer(std::nullptr_t) : buffer{nullptr} {}
  VmaBuffer(VmaAllocator allocator, vk::Buffer buffer, VmaAllocation allocation,
            VmaAllocationInfo alloc_info)
      : allocator{allocator}, buffer{buffer}, allocation{allocation},
        alloc_info{alloc_info} {}

  ~VmaBuffer() {
    if (allocator) {
      vmaDestroyBuffer(allocator, buffer, allocation);
    }
  }

  VmaBuffer(const VmaBuffer &) = delete;
  VmaBuffer &operator=(const VmaBuffer &) = delete;
  VmaBuffer(VmaBuffer &&other)
      : allocator{std::exchange(other.allocator, nullptr)},
        buffer{std::move(other.buffer)},
        allocation{std::move(other.allocation)},
        alloc_info{std::move(other.alloc_info)} {}

  VmaBuffer &operator=(VmaBuffer &&rhs) {
    using std::swap;
    swap(buffer, rhs.buffer);
    swap(allocation, rhs.allocation);
    swap(alloc_info, rhs.alloc_info);
    return *this;
  }

  vk::Buffer get_buffer() const { return buffer; }
  VmaAllocation get_allocation() const { return allocation; }
  VmaAllocationInfo get_alloc_info() const { return alloc_info; }

  vk::ResultValueType<VmaMappedBuffer>::type map_memory();

private:
  friend class VmaMappedBuffer;
  VmaAllocator allocator = nullptr;
  vk::Buffer buffer = nullptr;
  VmaAllocation allocation;
  VmaAllocationInfo alloc_info;
};

class VkMemAllocator {
public:
  static VkMemAllocator create(vk::raii::Instance &inst,
                               vk::raii::PhysicalDevice &phys_dev,
                               vk::raii::Device &dev);

  VkMemAllocator(VmaAllocator allocator);
  ~VkMemAllocator();

  VkMemAllocator(const VkMemAllocator &) = delete;
  VkMemAllocator &operator=(const VkMemAllocator &) = delete;
  VkMemAllocator(VkMemAllocator &&other)
      : allocator{std::exchange(other.allocator, nullptr)} {}
  VkMemAllocator &operator=(VkMemAllocator &&rhs) {
    using std::swap;
    swap(allocator, rhs.allocator);
    return *this;
  }

  operator VmaAllocator() const { return allocator; }
  VmaAllocator operator*() const { return allocator; }

  VmaBuffer createBuffer(const vk::BufferCreateInfo &create_info,
                         const VmaAllocationCreateInfo &alloc_info);

private:
  VmaAllocator allocator;
};
} // namespace vkvideo
