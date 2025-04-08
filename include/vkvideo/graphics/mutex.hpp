#pragma once

#include <vkvideo/core/types.hpp>

#include <mutex>

namespace vkvideo {
class QueueMutexMap {
public:
  void lock(u32 qfi, u32 qi);
  void unlock(u32 qfi, u32 qi);

  template <class T = std::scoped_lock<std::mutex>> T acquire(u32 qfi, u32 qi) {
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
} // namespace vkvideo
