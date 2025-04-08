#pragma once

#include "vkvideo/graphics/tlsem.hpp"

#include <mcpp/unique_any.hpp>
#include <vkvideo/core/types.hpp>
#include <vkvideo/graphics/mutex.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <mutex>

namespace vkvideo {
class TempCommandPools {
public:
  TempCommandPools() = default;

  // the mutex makes this class non-moveable...
  void init(vk::raii::Device &device, QueueMutexMap &queue_muts);

  vk::raii::CommandBuffer begin(i32 qf_idx);
  std::pair<std::shared_ptr<TimelineSemaphore>, u64>
  end(vk::raii::CommandBuffer cmd_buf, i32 qf_idx,
      mcpp::unique_any free_on_finish = {},
      const vk::ArrayProxyNoTemporaries<vk::SemaphoreSubmitInfo> &wait_sems =
          {});

  void garbage_collect(i64 timeout = 0);

private:
  vk::raii::Device *device = nullptr;
  QueueMutexMap *queue_muts = nullptr;
  std::unordered_map<i32, vk::raii::CommandPool> cmd_pools;
  std::unordered_map<i32, vk::raii::Queue> queues;
  std::mutex mutex;

  struct TransferPoolOperation {
    std::shared_ptr<TimelineSemaphore> sem;
    u64 sem_value;
    vk::raii::CommandBuffer cmd_buf = nullptr;
    mcpp::unique_any free_on_finish;

    TransferPoolOperation() = default;
  };

  std::vector<TransferPoolOperation> operations;
};
} // namespace vkvideo
