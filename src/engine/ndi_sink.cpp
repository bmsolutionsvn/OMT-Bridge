#include "engine/ndi_sink.h"

#include <cstdint>
#include <utility>

#include <Processing.NDI.Lib.h>

#include "core/alpha_convert.h"
#include "engine/ndi_runtime.h"

namespace ob {

struct NdiSenderHolder {
  NDIlib_send_instance_t send = nullptr;
  ~NdiSenderHolder() {
    if (send) NDIlib_send_destroy(send);
  }
};

NdiSink::NdiSink() = default;
NdiSink::~NdiSink() = default;

// A sender cannot be reconfigured, so configure() closes and reopens when anything relevant
// changed, comparing against the previously stored config.
void NdiSink::configure(const Output& output, const std::string& advertisedName) {
  std::lock_guard l(mu_);
  // sender_ in the comparison means a failed open is retried by the next identical configure;
  // that is the only recovery path without a restart.
  const bool same = sender_ && config_.target == output.target && config_.codec == output.codec &&
                    config_.quality == output.quality && name_ == advertisedName;
  config_ = output;
  if (same) return;
  name_ = advertisedName;
  openLocked();
}

void NdiSink::openLocked() {
  sender_.reset();
  openNote_.clear();
  if (!ensureNdi()) {
    openNote_ = "the NDI runtime could not start";
    return;  // no sender: send() drops frames, which is correct for an unavailable target
  }
  NDIlib_send_create_t settings;
  settings.p_ndi_name = name_.c_str();
  settings.p_groups = nullptr;
  settings.clock_video = false;
  settings.clock_audio = false;
  std::unique_ptr<NdiSenderHolder> holder;
  try {
    holder = std::make_unique<NdiSenderHolder>();
  } catch (...) {
    openNote_ = "cannot open NDI output";
    return;
  }
  holder->send = NDIlib_send_create(&settings);
  if (!holder->send) {
    openNote_ = "cannot open NDI output";
    return;  // no sender: the row reports the note and stays not-live, and apply() survives
  }
  sender_ = std::move(holder);
}

void NdiSink::deliver(const std::shared_ptr<const Frame>& frame) {
  if (!frame || !frame->valid()) return;  // never index a plane the frame does not have
  std::lock_guard l(mu_);
  // No sender (open failed): send() drops the frame. The decision is still made here so the
  // test seam sees it, matching OmtSink.
  const bool alpha = frame->format.alpha && config_.alpha != AlphaMode::Off;
  if (alpha && frame->premultiplied) {
    // NDI is straight only, so the copy is the price; the hub's frame stays shared and untouched.
    scratch_ = *frame;  // reuses the buffer; the hub's frame stays shared and untouched
    toStraight(scratch_);
    send(scratch_, alpha);
    return;
  }
  send(*frame, alpha);
}

void NdiSink::send(const Frame& frame, bool alpha) {
  if (!sender_ || !sender_->send) return;
  NDIlib_video_frame_v2_t v;
  v.xres = frame.format.width;
  v.yres = frame.format.height;
  // UYVA is the UYVY plane followed by a width*height alpha plane, our canonical layout too.
  v.FourCC = alpha ? NDIlib_FourCC_type_UYVA : NDIlib_FourCC_type_UYVY;
  v.frame_rate_N = frame.format.fpsN;
  v.frame_rate_D = frame.format.fpsD;
  v.picture_aspect_ratio = 0.0f;  // square pixels
  v.frame_format_type = NDIlib_frame_format_type_progressive;
  v.timecode = NDIlib_send_timecode_synthesize;
  v.p_data = const_cast<uint8_t*>(frame.uyvy.data());
  v.p_metadata = nullptr;
  v.timestamp = frame.timestamp;
  // Without alpha this is the UYVY plane alone: the SDK sends line_stride * yres bytes.
  v.line_stride_in_bytes = frame.format.width * 2;
  NDIlib_send_send_video_v2(sender_->send, &v);
}

void NdiSink::deliverAudio(const std::shared_ptr<const AudioFrame>& frame) {
  if (!frame || !frame->valid()) return;
  std::lock_guard l(mu_);
  if (!sender_ || !sender_->send) return;
  NDIlib_audio_frame_v3_t a;
  a.sample_rate = frame->format.sampleRate;
  a.no_channels = frame->format.channels;
  a.no_samples = frame->samples;
  a.timecode = NDIlib_send_timecode_synthesize;
  a.FourCC = NDIlib_FourCC_audio_type_FLTP;  // our planar layout is NDI's FLTP
  a.p_data = reinterpret_cast<uint8_t*>(const_cast<float*>(frame->planar.data()));
  a.p_metadata = nullptr;
  a.timestamp = frame->timestamp;
  a.channel_stride_in_bytes = frame->samples * static_cast<int>(sizeof(float));
  NDIlib_send_send_audio_v3(sender_->send, &a);
}

std::string NdiSink::note() const {
  std::lock_guard l(mu_);
  return openNote_;
}

Tally NdiSink::tally() const {
  std::lock_guard l(mu_);
  NDIlib_tally_t t{};
  if (sender_ && sender_->send && NDIlib_send_get_tally(sender_->send, &t, 0))
    return Tally{t.on_program, t.on_preview};
  return {};
}

}  // namespace ob