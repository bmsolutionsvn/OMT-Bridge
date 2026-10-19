#include "engine/omt_sink.h"

#include <omthx/omthx.h>

namespace ob {

struct OmtSenderHolder {
  std::unique_ptr<omthx::Sender> sender;
};

namespace {
omthx::Quality qualityOf(Quality q) {
  switch (q) {
    case Quality::Low: return omthx::Quality::Low;
    case Quality::Medium: return omthx::Quality::Medium;
    case Quality::High: return omthx::Quality::High;
  }
  return omthx::Quality::Medium;
}
omthx::Codec codecOf(HxCodec c) { return c == HxCodec::H264 ? omthx::Codec::H264 : omthx::Codec::Hevc; }
}

OmtSink::OmtSink() = default;
OmtSink::~OmtSink() = default;

// A Sender cannot be reconfigured, so configure() closes and reopens when anything relevant
// changed, comparing against the previously stored config.
void OmtSink::configure(const Output& output, const std::string& advertisedName) {
  std::lock_guard l(mu_);
  const bool same = sender_ && config_.target == output.target && config_.codec == output.codec &&
                    config_.quality == output.quality && name_ == advertisedName;
  config_ = output;
  if (same) return;
  name_ = advertisedName;
  openLocked();
}

void OmtSink::openLocked() {
  sender_.reset();
  note_.clear();
  if (config_.target == OutputTarget::Ndi) {
    note_ = "NDI output needs the NDI adapter (M3)";
    return;  // no sender: deliver() drops frames, which is correct for an unavailable target
  }
  const auto mode = config_.target == OutputTarget::OmtHx ? omthx::Mode::Hx : omthx::Mode::Full;
  sender_ = std::make_unique<OmtSenderHolder>();
  try {
    sender_->sender = std::make_unique<omthx::Sender>(name_, mode, qualityOf(config_.quality),
                                                      codecOf(config_.codec));
  } catch (...) {
    sender_.reset();
    note_ = "cannot open OMT output";
    throw;
  }
}

void OmtSink::deliver(const std::shared_ptr<const Frame>& frame) {
  if (!frame || !frame->valid()) return;  // never index a plane the frame does not have
  std::lock_guard l(mu_);

  // The label describes THIS frame, so recompute it every time. The NDI/open note is not
  // per-frame and is left alone.
  if (config_.target != OutputTarget::Ndi) note_.clear();
  const bool hadAlpha = frame->format.alpha;
  bool alpha = hadAlpha && config_.alpha != AlphaMode::Off;
  if (hadAlpha && config_.alpha == AlphaMode::Off) {
    note_ = "alpha dropped";  // design 7.1: the operator chose to save bandwidth
  } else if (alpha && config_.target == OutputTarget::OmtHx && config_.codec == HxCodec::H264 &&
             frame->format.height > 1080) {
    alpha = false;
    note_ = "alpha dropped — H.264 above 1080p, use HEVC";
  }
  send(frame->format, frame->uyvy.data(), alpha, frame->premultiplied, frame->timestamp);
}

void OmtSink::send(const VideoFormat& format, const uint8_t* uyvy, bool alpha, bool premultiplied,
                   int64_t timestamp) {
  if (!sender_ || !sender_->sender) return;
  omthx::VideoFormat vf{format.width, format.height, format.fpsN, format.fpsD, alpha};
  const uint8_t* plane =
      alpha ? uyvy + static_cast<size_t>(format.width) * 2u * format.height : nullptr;
  const int alphaStride = static_cast<int>(format.width);
  sender_->sender->sendVideoUyvy(vf, uyvy, format.width * 2, plane, alphaStride, premultiplied,
                                 timestamp);
}

std::string OmtSink::note() const {
  std::lock_guard l(mu_);
  return note_;
}

}  // namespace ob
