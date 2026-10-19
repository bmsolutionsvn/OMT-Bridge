#include "engine/source_hub.h"

#include <utility>

namespace ob {

bool SourceHub::computeRunning() const { return refs_ > 0 || thumbnailWanted_; }

void SourceHub::maybeTransition() {
  RunningChanged cb;
  bool next = false;
  {
    std::lock_guard l(mu_);
    next = computeRunning();
    if (next == running_) return;
    running_ = next;
    // Copy the callback under the lock: a concurrent setOnRunningChanged must not reassign the
    // std::function while we are invoking it (that would be a data race, and can tear).
    cb = onRunningChanged_;
  }
  // Outside the lock: the callback starts or stops an adapter, which may call back in.
  if (cb) cb(next);
}

void SourceHub::addRef() {
  {
    std::lock_guard l(mu_);
    ++refs_;
  }
  maybeTransition();
}

void SourceHub::release() {
  {
    std::lock_guard l(mu_);
    if (refs_ > 0) --refs_;
  }
  maybeTransition();
}

int SourceHub::refs() const {
  std::lock_guard l(mu_);
  return refs_;
}

void SourceHub::setThumbnailWanted(bool wanted) {
  {
    std::lock_guard l(mu_);
    thumbnailWanted_ = wanted;
  }
  maybeTransition();
}

bool SourceHub::thumbnailWanted() const {
  std::lock_guard l(mu_);
  return thumbnailWanted_;
}

bool SourceHub::running() const {
  std::lock_guard l(mu_);
  return running_;
}

void SourceHub::setOnRunningChanged(RunningChanged cb) {
  std::lock_guard l(mu_);
  onRunningChanged_ = std::move(cb);
}

void SourceHub::publish(std::shared_ptr<const Frame> frame) {
  std::shared_ptr<const Frame> previous;
  {
    std::lock_guard l(mu_);
    previous = std::move(latest_);
    latest_ = std::move(frame);
    ++published_;
  }
  // `previous` is released here, outside the lock: if this hub held the last reference, dropping
  // it frees a full frame, and that must not happen while the mutex is held.
}

std::shared_ptr<const Frame> SourceHub::latest() const {
  std::lock_guard l(mu_);
  return latest_;
}

uint64_t SourceHub::framesPublished() const {
  std::lock_guard l(mu_);
  return published_;
}

}  // namespace ob
