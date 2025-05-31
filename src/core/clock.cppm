export module vkvideo.core:clock;

import std;
import :types;

export namespace vkvideo {
class Clock {
public:
  virtual ~Clock() = default;

  virtual i64 get_time() = 0;
};

class ClockPtr : public std::unique_ptr<Clock>, public Clock {
public:
  using std::unique_ptr<Clock>::unique_ptr;

  i64 get_time() override { return get()->get_time(); }
};

class SteadyClock : public Clock {
public:
  SteadyClock() : start_time{now()} {}

  static std::chrono::steady_clock::time_point now() {
    return std::chrono::steady_clock::now();
  }

  i64 get_time() override {
    i64 rel_time =
        paused ? 0 : std::chrono::nanoseconds{now() - start_time}.count();
    return rel_time + offset;
  }

  void seek(i64 amount) { offset += amount; }
  void seek_to(i64 time) { seek(time - get_time()); }

  void pause() {
    offset = get_time();
    start_time = now();
    paused = true;
  }

  void play() {
    start_time = now();
    paused = false;
  }

  bool is_paused() const { return paused; }

private:
  std::chrono::steady_clock::time_point start_time;
  i64 offset = 0;
  bool paused = false;
};
} // namespace vkvideo
