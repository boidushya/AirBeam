// Copyright (c) 2025 ChenKS12138

#include "fifo.h"

#include <algorithm>
#include <cstring>

namespace AirBeamCore {
namespace raop {
size_t ConcurrentByteFIFO::Write(const uint8_t* data, size_t length,
                                 std::chrono::milliseconds timeout) {
  size_t written = 0;
  while (written < length) {
    std::unique_lock<std::mutex> lock(mutex_);
    bool timeout_occurred = false;
    if (size_ == capacity_) {
      if (timeout == std::chrono::milliseconds(0)) {
        not_full_cv_.wait(lock, [this]() { return size_ < capacity_; });
      } else {
        timeout_occurred = !not_full_cv_.wait_for(
            lock, timeout, [this]() { return size_ < capacity_; });
      }
    }

    if (timeout_occurred) {
      break;
    }

    size_t available = capacity_ - size_;
    size_t to_write = std::min(length - written, available);
    size_t first_chunk = std::min(to_write, capacity_ - head_);
    memcpy(buffer_.data() + head_, data + written, first_chunk);
    if (to_write > first_chunk) {
      memcpy(buffer_.data(), data + written + first_chunk,
             to_write - first_chunk);
    }
    head_ = (head_ + to_write) % capacity_;
    size_ += to_write;
    written += to_write;
    lock.unlock();
    not_empty_cv_.notify_one();
  }
  return written;
}

size_t ConcurrentByteFIFO::Read(uint8_t* data, size_t length,
                                std::chrono::milliseconds timeout) {
  size_t read_count = 0;
  while (read_count < length) {
    std::unique_lock<std::mutex> lock(mutex_);
    bool timeout_occurred = false;
    if (size_ == 0) {
      if (timeout == std::chrono::milliseconds(0)) {
        not_empty_cv_.wait(lock, [this]() { return size_ > 0; });
      } else {
        timeout_occurred = !not_empty_cv_.wait_for(
            lock, timeout, [this]() { return size_ > 0; });
      }
    }

    if (timeout_occurred) {
      break;
    }

    size_t available = size_;
    size_t to_read = std::min(length - read_count, available);
    size_t first_chunk = std::min(to_read, capacity_ - tail_);
    memcpy(data + read_count, buffer_.data() + tail_, first_chunk);
    if (to_read > first_chunk) {
      memcpy(data + read_count + first_chunk, buffer_.data(),
             to_read - first_chunk);
    }
    tail_ = (tail_ + to_read) % capacity_;
    size_ -= to_read;
    read_count += to_read;
    lock.unlock();
    not_full_cv_.notify_one();
  }
  return read_count;
}

bool ConcurrentByteFIFO::Full() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return size_ == capacity_;
}
}  // namespace raop
}  // namespace AirBeamCore
