#pragma once

namespace vkvideo {
template <class T> struct remove_reference {
  using type = T;
};
template <class T> struct remove_reference<T &> {
  using type = T;
};
template <class T> struct remove_reference<T &&> {
  using type = T;
};
template <class T>
using remove_reference_t = typename remove_reference<T>::type;

// https://www.foonathan.net/2020/09/move-forward/
#define VKVIDEO_MOV(...)                                                       \
  static_cast<remove_reference_t<decltype(__VA_ARGS__)> &&>(__VA_ARGS__)
#define VKVIDEO_FWD(...) static_cast<decltype(__VA_ARGS__) &&>(__VA_ARGS__)
} // namespace vkvideo
