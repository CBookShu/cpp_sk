#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <shared_mutex>
#include <string_view>
#include <utility>
#include <vector>
#include <string>

template <typename T> class mutex_type {
  public:
    template <typename... Args>
    mutex_type(Args &&...args) : data_(std::forward<Args>(args)...) {}
  
    class Guard {
      std::lock_guard<T> guard_;
      mutex_type *p_;
  
    public:
      Guard(mutex_type *m) : guard_(m->mux_), p_(m) {}
  
      T *operator->() { return &(p_->data_); }
  
      T *get() { return &(p_->data_); }
    };
  
    [[nodiscard]]
    Guard write() {
      return Guard(this);
    }
  
  private:
    std::mutex mux_;
    T data_;
  };
  
  template <typename T> class shared_mutex_type {
  public:
    template <typename... Args>
    shared_mutex_type(Args &&...args) : data_(std::forward<Args>(args)...) {}
  
    class WriteGuard {
      std::lock_guard<std::shared_mutex> guard_;
      shared_mutex_type *p_;
  
    public:
      WriteGuard(shared_mutex_type *m) : guard_(m->smux_), p_(m) {}
  
      T *operator->() { return &(p_->data_); }
  
      T *get() { return &(p_->data_); }
    };
  
    class ReadGuard {
      std::shared_lock<std::shared_mutex> guard_;
      shared_mutex_type *p_;
  
    public:
      ReadGuard(shared_mutex_type *m) : guard_(m->smux_), p_(m) {}
  
      const T *operator->() { return &(p_->data_); }
  
      const T *get() { return &(p_->data_); }
    };
  
    WriteGuard write() { return WriteGuard(this); }
  
    ReadGuard read() { return ReadGuard(this); }
  
  private:
    std::shared_mutex smux_;
    T data_;
  };
  
  struct string_hash {
    using is_transparent = void;
    [[nodiscard]] size_t operator()(const char *txt) const {
      return std::hash<std::string_view>{}(txt);
    }
    [[nodiscard]] size_t operator()(std::string_view txt) const {
      return std::hash<std::string_view>{}(txt);
    }
    [[nodiscard]] size_t operator()(const std::string &txt) const {
      return std::hash<std::string>{}(txt);
    }
  };
  
  class spinlock_mutex {
    std::atomic_flag flag;
  
  public:
    spinlock_mutex() : flag{false} {}
    void lock() {
      while (flag.test_and_set(std::memory_order_acquire))
        ;
    }
    void unlock() { flag.clear(std::memory_order_release); }

    bool trylock() {
      return !flag.test_and_set(std::memory_order_acquire);
    }
  };

struct algo {
  static constexpr std::vector<std::string_view> split(std::string_view s, std::string_view div = " ") {
    std::vector<std::string_view> vs;
    int pos = 0;
    do
    {
        auto d = s.find(div, pos);
        if (d == std::string::npos) {
            if (pos != s.size()) {
                vs.push_back(s.substr(pos));
            }
            break;
        }
        vs.push_back(s.substr(pos, d - pos));
        pos = d + 1;
    } while (true);
    return vs;
  }

  static constexpr std::pair<std::string_view, std::string_view> split_one(std::string_view s, std::string_view div = " ") {
    auto pos = s.find(div);
    if (pos == std::string_view::npos) {
      return std::make_pair(s, "");
    } else {
      return std::make_pair(s.substr(0, pos), s.substr(pos+1));
    }
  }

  static char* str_malloc(std::string_view s) {
    if(s.empty()) return nullptr;
    char* str = (char*)std::malloc(s.size() + 1);
    std::copy(s.begin(), s.end(), str);
    str[s.size()] = 0;
    return str;
  }

  template <typename T>
  static void safe_free(T * buf) {
    if (buf) {
      std::free((void*)buf);
    }
  }
};