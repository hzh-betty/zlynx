#include "stealable_queue_bitmap.h"

namespace zcoroutine {

StealableQueueBitmap::StealableQueueBitmap(size_t worker_count)
    : worker_count_(worker_count), words_((worker_count + 63) / 64) {}

void StealableQueueBitmap::set(size_t worker_id) {
  if (worker_id >= worker_count_) {
    return;
  }
  const size_t word = worker_id / 64;
  const uint64_t mask = (uint64_t{1} << (worker_id % 64));
  words_[word].v.fetch_or(mask, std::memory_order_relaxed);
}

void StealableQueueBitmap::clear(size_t worker_id) {
  if (worker_id >= worker_count_) {
    return;
  }
  const size_t word = worker_id / 64;
  const uint64_t mask = (uint64_t{1} << (worker_id % 64));
  words_[word].v.fetch_and(~mask, std::memory_order_relaxed);
}

bool StealableQueueBitmap::test(size_t worker_id) const {
  if (worker_id >= worker_count_) {
    return false;
  }
  const size_t word = worker_id / 64;
  const uint64_t mask = (uint64_t{1} << (worker_id % 64));
  return (words_[word].v.load(std::memory_order_relaxed) & mask) != 0;
}

bool StealableQueueBitmap::any() const {
  for (const auto &w : words_) {
    if (w.v.load(std::memory_order_relaxed) != 0) {
      return true;
    }
  }
  return false;
}

int StealableQueueBitmap::find_non_stealable(size_t start) const {
  if (worker_count_ == 0) {
    return -1;
  }
  start %= worker_count_;
  int id = find_zero_in_range(start, worker_count_);
  if (id >= 0) {
    return id;
  }
  return find_zero_in_range(0, start);
}

int StealableQueueBitmap::find_victim(int self_id) const {
  if (worker_count_ <= 1) {
    return -1;
  }
  if (self_id < 0 || static_cast<size_t>(self_id) >= worker_count_) {
    return -1;
  }

  // 扫描起点：从 (self_id + 1) % N 开始，避免所有线程都从 0 扫描导致热点。
  const size_t start = (static_cast<size_t>(self_id) + 1) % worker_count_;
  int victim =
      find_in_range(start, worker_count_, static_cast<size_t>(self_id));
  if (victim >= 0) {
    return victim;
  }
  return find_in_range(0, start, static_cast<size_t>(self_id));
}

int StealableQueueBitmap::find_in_range(size_t from, size_t to,
                                        size_t self_id) const {
  if (from >= to) {
    return -1;
  }

  size_t i = from;
  while (i < to) {
    const size_t word_index = i / 64;
    const size_t bit_offset = i % 64;

    uint64_t word = words_[word_index].v.load(std::memory_order_relaxed);

    // 只保留 [bit_offset, 63] 范围内的 bit（用于实现从任意 bit 起点扫描）。
    word &= (~uint64_t{0} << bit_offset);

    // 若扫描区间在该 word 结束，则清掉 >= to 的 bit。
    const size_t last = to - 1;
    if (word_index == last / 64) {
      const size_t last_off = last % 64;
      const uint64_t end_mask = (last_off == 63)
                                    ? ~uint64_t{0}
                                    : ((uint64_t{1} << (last_off + 1)) - 1);
      word &= end_mask;
    }

    // 排除自己，避免无意义的自窃取。
    if (self_id / 64 == word_index) {
      word &= ~(uint64_t{1} << (self_id % 64));
    }

    if (word != 0) {
      // 直接使用 ctz 查找最低位 1（word != 0 时才可调用，避免 UB）。
      const int bit = __builtin_ctzll(static_cast<unsigned long long>(word));
      const size_t victim = word_index * 64 + static_cast<size_t>(bit);
      if (victim < worker_count_) {
        return static_cast<int>(victim);
      }
      return -1;
    }

    // 跳到下一个 word 的起始 bit。
    i = (word_index + 1) * 64;
  }

  return -1;
}

int StealableQueueBitmap::find_zero_in_range(size_t from, size_t to) const {
  if (from >= to) {
    return -1;
  }

  size_t i = from;
  while (i < to) {
    const size_t word_index = i / 64;
    const size_t bit_offset = i % 64;

    uint64_t word = words_[word_index].v.load(std::memory_order_relaxed);
    uint64_t inv = ~word;

    // 只保留 [bit_offset, 63] 范围内的 bit。
    inv &= (~uint64_t{0} << bit_offset);

    // 若扫描区间在该 word 结束，则清掉 >= to 的 bit。
    const size_t last = to - 1;
    if (word_index == last / 64) {
      const size_t last_off = last % 64;
      const uint64_t end_mask = (last_off == 63)
                                    ? ~uint64_t{0}
                                    : ((uint64_t{1} << (last_off + 1)) - 1);
      inv &= end_mask;
    }

    if (inv != 0) {
      // inv != 0 时才可调用 ctz，避免 UB。
      const int bit = __builtin_ctzll(static_cast<unsigned long long>(inv));
      const size_t wid = word_index * 64 + static_cast<size_t>(bit);
      if (wid < worker_count_) {
        return static_cast<int>(wid);
      }
      return -1;
    }

    i = (word_index + 1) * 64;
  }

  return -1;
}

} // namespace zcoroutine
