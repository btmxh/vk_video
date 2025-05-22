export module vkvideo.graphics:temppools;

import vkvideo.core;
import std;
import vulkan_hpp;
import :queues;
import :tlsem;
#include <cassert>

export namespace vkvideo::graphics {
class TempCommandPools {
public:
  TempCommandPools() = default;

  // the mutex makes this class non-moveable...
  void init(vk::raii::Device &device, QueueManager &queues) {
    this->device = &device;
    this->queues = &queues;
  }

  vk::raii::CommandBuffer begin(i32 qf_idx) {
    assert(qf_idx != vk::QueueFamilyIgnored);
    std::scoped_lock _lck{mutex};
    auto it = cmd_pools.find(qf_idx);
    if (it == cmd_pools.end()) {
      std::tie(it, std::ignore) = cmd_pools.emplace(
          qf_idx,
          vk::raii::CommandPool{
              *device, vk::CommandPoolCreateInfo{
                           .flags = vk::CommandPoolCreateFlagBits::eTransient,
                           .queueFamilyIndex = static_cast<u32>(qf_idx),
                       }});
    }
    vk::raii::CommandBuffers buffers{
        *device, vk::CommandBufferAllocateInfo{
                     .commandPool = it->second,
                     .level = vk::CommandBufferLevel::ePrimary,
                     .commandBufferCount = 1,
                 }};
    return std::move(buffers[0]);
  }

  std::pair<std::shared_ptr<TimelineSemaphore>, u64>
  end(vk::raii::CommandBuffer cmd_buf, i32 qf_idx,
      UniqueAny free_on_finish = {},
      const vk::ArrayProxy<vk::SemaphoreSubmitInfo> &wait_sems = {},
      const vk::ArrayProxy<vk::SemaphoreSubmitInfo> &signal_sems = {},
      vk::PipelineStageFlags2 additional_stage_mask = {}) {
    static const u64 sem_value = 1;
    std::scoped_lock _lck{mutex};
    auto name = std::format("task_sem[{}-{}]", qf_idx, 367);
    auto sem = std::make_shared<TimelineSemaphore>(*device, 0, name.c_str());

    auto &op = operations.emplace_back();
    op.sem = sem;
    op.sem_value = sem_value;
    op.cmd_buf = std::move(cmd_buf);
    op.free_on_finish = std::move(free_on_finish);

    vk::CommandBufferSubmitInfo cmd_buf_si{
        .commandBuffer = *op.cmd_buf,
    };

    std::vector<vk::SemaphoreSubmitInfo> signal_sem_infos{signal_sems.begin(),
                                                          signal_sems.end()};
    signal_sem_infos.push_back(vk::SemaphoreSubmitInfo{
        .semaphore = *op.sem,
        .value = static_cast<u64>(sem_value),
        .stageMask =
            vk::PipelineStageFlagBits2::eBottomOfPipe | additional_stage_mask,
    });

    auto [queue_lock, queue] = queues->get_queue(qf_idx);
    queue.submit2(vk::SubmitInfo2{}
                      .setCommandBufferInfos(cmd_buf_si)
                      .setWaitSemaphoreInfos(wait_sems)
                      .setSignalSemaphoreInfos(signal_sem_infos));

    return {sem, sem_value};
  }

  void garbage_collect(i64 timeout = 0) {
    std::scoped_lock _lck{mutex};

    std::vector<vk::Semaphore> wait_sems;
    std::vector<u64> wait_sem_values;

    for (auto &op : operations) {
      wait_sems.push_back(*op.sem);
      wait_sem_values.push_back(static_cast<u64>(op.sem_value));
    }

    if (!wait_sems.empty()) {
      auto ignore =
          device->waitSemaphores(vk::SemaphoreWaitInfo{}
                                     .setSemaphores(wait_sems)
                                     .setValues(wait_sem_values)
                                     .setFlags(vk::SemaphoreWaitFlagBits::eAny),
                                 timeout);

      std::erase_if(operations, [&](TransferPoolOperation &op) {
        return op.sem->getCounterValue() >= op.sem_value;
      });
    }
  }

private:
  vk::raii::Device *device = nullptr;
  QueueManager *queues = nullptr;
  std::unordered_map<i32, vk::raii::CommandPool> cmd_pools;
  std::mutex mutex;

  struct TransferPoolOperation {
    std::shared_ptr<TimelineSemaphore> sem;
    u64 sem_value;
    vk::raii::CommandBuffer cmd_buf = nullptr;
    UniqueAny free_on_finish;

    TransferPoolOperation() = default;
  };

  std::vector<TransferPoolOperation> operations;
};
} // namespace vkvideo::graphics

namespace vkvideo::graphics {} // namespace vkvideo::graphics
