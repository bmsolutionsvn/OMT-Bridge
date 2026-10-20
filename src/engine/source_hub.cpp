#include "engine/source_hub.h"

#include <chrono>
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

void SourceHub::storeFrame(std::shared_ptr<Frame> frame) {
  if (frame && frame->format.alpha) {
    // One scan per source frame, shared by every output (design 6.4).
    const size_t offset = frame->alphaOffset();
    const size_t bytes = frame->alphaBytes();
    const uint8_t* plane = frame->uyvy.data() + offset;
    if (alpha_.shouldDrop(true, plane, bytes, clock_())) {
      // Drop the plane without copying: shrinking a vector does not move the picture, and
      // Frame::valid() requires uyvy.size() == lumaBytes() + alphaBytes() for the new format.
      frame->format.alpha = false;
      frame->premultiplied = false;  // nothing to premultiply once alpha is gone
      frame->uyvy.resize(frame->lumaBytes());
    }
  } else {
    alpha_.reset();
  }

  // Consumers only ever see shared_ptr<const Frame>; the hub mutated the object above, before storing it.
  std::shared_ptr<const Frame> published = std::move(frame);
  std::shared_ptr<const Frame> previous;
  {
    std::lock_guard l(mu_);
    previous = std::move(latest_);
    latest_ = std::move(published);
    ++published_;
  }
  // `previous` is released here, outside the lock: dropping the last reference frees a full frame.
}

std::shared_ptr<const Frame> SourceHub::latest() const {
  std::lock_guard l(mu_);
  return latest_;
}

uint64_t SourceHub::framesPublished() const {
  std::lock_guard l(mu_);
  return published_;
}

void SourceHub::storeAudio(std::shared_ptr<AudioFrame> frame) {
  std::shared_ptr<const AudioFrame> published = std::move(frame);
  std::shared_ptr<const AudioFrame> previous;
  {
    std::lock_guard l(mu_);
    previous = std::move(latestAudio_);
    latestAudio_ = std::move(published);
    ++audioPublished_;
  }
  // `previous` is released here, outside the lock (same reason as storeFrame).
}

std::shared_ptr<const AudioFrame> SourceHub::latestAudio() const {
  std::lock_guard l(mu_);
  return latestAudio_;
}

uint64_t SourceHub::audioFramesPublished() const {
  std::lock_guard l(mu_);
  return audioPublished_;
}

}  // namespace ob
