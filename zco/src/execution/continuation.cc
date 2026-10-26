#include "execution/continuation.h"
#include <cstring>
#ifdef ZCO_BENCHMARK_METRICS
#include <atomic>
#endif

namespace zco {
namespace detail {
#ifdef ZCO_BENCHMARK_METRICS
namespace {
std::atomic<uint64_t> copied_bytes{0};
}

uint64_t snapshot_bytes_copied() {
    return copied_bytes.load(std::memory_order_relaxed);
}
#endif
Continuation::Continuation(StackArena &arena, void (*entry)()) : arena_(arena) {
    if (arena.shared())
        stack_ = &arena.shared_slot(); // 共享栈槽位由 Arena 持有，挂起后保存快照、恢复前还原。
    else {
        independent_ = arena.acquire_independent();
        stack_ = independent_.get(); // 独立栈由 Continuation 持有，挂起后无需保存快照。
    }
    make_context(context_, stack_->data(), stack_->size(), entry);
}

Continuation::~Continuation() {
    arena_.release_independent(std::move(independent_));
}

void Continuation::resume(NativeContext &caller) {
    if (!snapshot_.empty()) {
#ifdef ZCO_BENCHMARK_METRICS
        copied_bytes.fetch_add(snapshot_.size(), std::memory_order_relaxed);
#endif
        std::memcpy(stack_->data() + stack_->size() - snapshot_.size(),
                    snapshot_.data(), snapshot_.size());
    }
    switch_context(caller, context_);
}

void Continuation::suspend(NativeContext &caller) {
    switch_context(context_, caller);
    if (interruption_) {
        auto error = std::move(interruption_);
        interruption_ = {};
        std::rethrow_exception(error);
    }
}

void Continuation::interrupt(NativeContext &caller,
                             std::exception_ptr failure) {
    interruption_ = std::move(failure);

    // 挂起的栈仍驻留在原位置，恢复旧快照反而会覆盖当前栈内容。
    switch_context(caller, context_);
}

void Continuation::save() {
    if (independent_)
        return;
    char *sp = stack_pointer(context_);
    char *end = stack_->data() + stack_->size();
    if (sp < stack_->data() || sp > end)
        throw std::logic_error("Stack snapshot outside stack bounds");
    snapshot_.assign(sp, end);
#ifdef ZCO_BENCHMARK_METRICS
    copied_bytes.fetch_add(snapshot_.size(), std::memory_order_relaxed);
#endif
}
} // namespace detail
} // namespace zco
