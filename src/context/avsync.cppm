export module vkvideo.context:avsync;

import std;
import vkvideo.core;

export namespace vkvideo::context {
template <class T> class WeightedRunningAvg {
public:
  WeightedRunningAvg(i32 max_count, double coeff)
      : coeff{coeff}, max_count{max_count} {}

  std::optional<T> update(T value) {
    cum_value = value + coeff * cum_value;
    if (count < max_count) {
      ++count;
      return std::nullopt;
    }

    return cum_value * (1 - coeff);
  }

  void reset() {
    cum_value = 0;
    count = 0;
  }

private:
  T cum_value = 0;
  i32 count = 0, max_count = 0;
  double coeff = 0.0;
};
}; // namespace vkvideo::context
