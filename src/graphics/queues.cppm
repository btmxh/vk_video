export module vkvideo.graphics:queues;

import std;
import vkvideo.core;
import vulkan_hpp;
import :vku;

export namespace vkvideo::graphics {

u64 calc_key(u32 qfi, u32 qi) { return (u64)qfi << 32 | (u64)qi; }

class QueueMutexMap {
public:
  void lock(u32 qfi, u32 qi) {
    std::scoped_lock _lck{main_mutex};
    get_mutex(_lck, qfi, qi).lock();
  }
  void unlock(u32 qfi, u32 qi) {
    std::scoped_lock _lck{main_mutex};
    get_mutex(_lck, qfi, qi).unlock();
  }

  template <class T = std::scoped_lock<std::mutex>>
  T acquire(u32 qfi, u32 qi = 0) {
    std::scoped_lock _lck{main_mutex};
    return T{get_mutex(_lck, qfi, qi)};
  }

private:
  std::mutex main_mutex;
  std::unordered_map<u64, std::unique_ptr<std::mutex>> mutexes;

  std::mutex &get_mutex(std::scoped_lock<std::mutex> &main_lock, u32 qfi,
                        u32 qi) {
    if (!mutexes.contains(calc_key(qfi, qi))) {
      mutexes[calc_key(qfi, qi)] = std::make_unique<std::mutex>();
    }
    return *mutexes[calc_key(qfi, qi)];
  }
};

class QueueManager {
public:
  void init(vk::raii::Device &device, u32 qf_graphics, u32 qf_compute,
            u32 qf_transfer, std::vector<u32> qf_video) {
    this->qf_graphics = qf_graphics;
    this->qf_compute = qf_compute;
    this->qf_transfer = qf_transfer;
    this->qf_video = std::move(qf_video);

    queues.clear();
    auto add_queue = [&](std::string queue_name, u32 qf_index,
                         u32 q_index = 0) {
      auto key = calc_key(qf_index, q_index);
      if (queues.contains(key))
        return;

      vk::raii::Queue queue{device, qf_index, q_index};
      set_debug_label(device, *queue, queue_name.c_str());
      queues.emplace(key, std::move(queue));
    };

    add_queue("graphics_queue", qf_graphics);
    add_queue("compute_queue", qf_compute);
    add_queue("transfer_queue", qf_transfer);
    for (auto qfv : qf_video)
      add_queue(std::format("video_queue_{}", qfv), qfv);
  }

  QueueMutexMap &get_mutexes() { return mutexes; }

  u32 get_qf_graphics() const { return qf_graphics; }
  u32 get_qf_compute() const { return qf_compute; }
  u32 get_qf_transfer() const { return qf_transfer; }

  template <class T = std::unique_lock<std::mutex>>
  std::pair<T, vk::raii::Queue &> get_graphics_queue() {
    return get_queue(qf_graphics);
  }

  template <class T = std::unique_lock<std::mutex>>
  std::pair<T, vk::raii::Queue &> get_compute_queue() {
    return get_queue(qf_compute);
  }

  template <class T = std::unique_lock<std::mutex>>
  std::pair<T, vk::raii::Queue &> get_transfer_queue() {
    return get_queue(qf_transfer);
  }

  template <class T = std::unique_lock<std::mutex>>
  std::pair<T, vk::raii::Queue &> get_queue(u32 qf_idx, u32 q_idx = 0) {
    return {mutexes.acquire<T>(qf_transfer),
            queues.at(calc_key(qf_idx, q_idx))};
  }

  std::span<const u32> get_video_qfs() const { return qf_video; }

private:
  QueueMutexMap mutexes;
  u32 qf_graphics;
  u32 qf_compute;
  u32 qf_transfer;
  std::vector<u32> qf_video;

  std::unordered_map<u64, vk::raii::Queue> queues;
};
} // namespace vkvideo::graphics
