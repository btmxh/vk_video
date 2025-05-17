export module vkvideo.graphics:tlsem;

import vkvideo.core;
import vulkan_hpp;
import std;

import :vku;

export namespace vkvideo::graphics {
class TimelineSemaphore : public vk::raii::Semaphore {
public:
  TimelineSemaphore(std::nullptr_t) : Semaphore(nullptr) {};
  TimelineSemaphore(vk::raii::Device &device, u64 value, const char *name)
      : Semaphore([&] {
          vk::SemaphoreTypeCreateInfo info{
              .semaphoreType = vk::SemaphoreType::eTimeline,
              .initialValue = value,
          };

          return vk::raii::Semaphore{device,
                                     vk::SemaphoreCreateInfo{.pNext = &info}};
        }()) {
    set_debug_label(device, **this, name);
  };

  TimelineSemaphore(vk::raii::Semaphore &&sem) : Semaphore(std::move(sem)) {};
  TimelineSemaphore &operator=(vk::raii::Semaphore &&sem) {
    Semaphore::operator=(std::move(sem));
    return *this;
  };

  TimelineSemaphore &operator=(std::nullptr_t) {
    Semaphore::operator=(nullptr);
    return *this;
  };

  vk::Result wait(u64 value, i64 timeout) {
    vk::Semaphore handle = **this;
    vk::SemaphoreWaitInfo wait_info{
        .semaphoreCount = 1,
        .pSemaphores = &handle,
        .pValues = &value,
    };
    return this->getDevice().waitSemaphores(&wait_info,
                                            static_cast<u64>(timeout));
  }

  u64 get_value() { return this->getDevice().getSemaphoreCounterValue(**this); }
};

}; // namespace vkvideo::graphics
