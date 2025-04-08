#include "vkvideo/graphics/tx.hpp"

#include "vkvideo/graphics/mutex.hpp"

#include <vulkan/vulkan_enums.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_structs.hpp>

#include <iostream>

namespace vkvideo {
void TempCommandPools::init(vk::raii::Device &device,
                            QueueMutexMap &queue_muts) {
  this->device = &device;
  this->queue_muts = &queue_muts;
}

vk::raii::CommandBuffer TempCommandPools::begin(i32 qf_idx) {
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

std::pair<std::shared_ptr<TimelineSemaphore>, u64> TempCommandPools::end(
    vk::raii::CommandBuffer cmd_buf, i32 qf_idx,
    mcpp::unique_any free_on_finish,
    const vk::ArrayProxyNoTemporaries<vk::SemaphoreSubmitInfo> &wait_sems) {
  static const u64 sem_value = 1;
  std::scoped_lock _lck{mutex};
  auto it = queues.find(qf_idx);
  if (it == queues.end()) {
    std::tie(it, std::ignore) =
        queues.emplace(qf_idx, device->getQueue(qf_idx, 0));
  }

  auto sem = std::make_shared<TimelineSemaphore>(*device, 0);

  auto &op = operations.emplace_back();
  op.sem = sem;
  op.sem_value = sem_value;
  op.cmd_buf = std::move(cmd_buf);
  op.free_on_finish = std::move(free_on_finish);

  auto _lock = queue_muts->template acquire<>(qf_idx, 0);

  vk::CommandBufferSubmitInfo cmd_buf_si{
      .commandBuffer = *op.cmd_buf,
  };

  vk::SemaphoreSubmitInfo sem_si{
      .semaphore = *op.sem,
      .value = static_cast<u64>(sem_value),
  };

  std::cout << wait_sems.size() << std::endl;

  it->second.submit2(vk::SubmitInfo2{}
                         .setCommandBufferInfos(cmd_buf_si)
                         .setWaitSemaphoreInfos(wait_sems)
                         .setSignalSemaphoreInfos(sem_si));

  return {sem, sem_value};
}

void TempCommandPools::garbage_collect(i64 timeout) {
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
} // namespace vkvideo
