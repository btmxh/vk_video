#include "vkvideo/graphics/mutex.hpp"

namespace vkvideo {
void QueueMutexMap::lock(u32 qfi, u32 qi) {
  std::scoped_lock _lck{main_mutex};
  get_mutex(_lck, qfi, qi).lock();
}

void QueueMutexMap::unlock(u32 qfi, u32 qi) {
  std::scoped_lock _lck{main_mutex};
  get_mutex(_lck, qfi, qi).unlock();
}
} // namespace vkvideo
