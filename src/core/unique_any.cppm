export module vkvideo.core:unique_any;

import std;

export namespace vkvideo {

class UniqueAny {
public:
  constexpr UniqueAny() = default;
  constexpr UniqueAny(UniqueAny &&other) noexcept = default;
  constexpr UniqueAny &operator=(UniqueAny &&rhs) noexcept = default;

  UniqueAny(const UniqueAny &) noexcept = delete;
  UniqueAny &operator=(const UniqueAny &) noexcept = delete;

  template <class T>
  UniqueAny(T &&value)
      : impl{std::make_unique<StorageImpl<T>>(std::forward<T>(value))} {}

  template <class T> UniqueAny &operator=(T &&value) {
    impl = std::make_unique<StorageImpl<T>>(std::forward<T>(value));
    return *this;
  }

private:
  struct Storage {
    virtual ~Storage() = default;
  };
  template <class T> struct StorageImpl : Storage {
    T value;
    StorageImpl(T &&value) : value{std::forward<T>(value)} {}
    ~StorageImpl() = default;
  };

  std::unique_ptr<Storage> impl;
};
} // namespace vkvideo
