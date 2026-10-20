#include "engine/ndi_source.h"

#include <chrono>
#include <cstring>
#include <exception>
#include <utility>

#include <Processing.NDI.Lib.h>

#include "engine/ndi_runtime.h"

namespace ob {

namespace {

constexpr uint32_t kFindTimeoutMs = 200;
constexpr uint32_t kCaptureTimeoutMs = 200;
constexpr auto kOfflineAfter = std::chrono::seconds(2);

struct Finder {
  NDIlib_find_instance_t instance = nullptr;
  ~Finder() {
    if (instance) NDIlib_find_destroy(instance);
  }
};

struct Receiver {
  NDIlib_recv_instance_t instance = nullptr;
  ~Receiver() {
    if (instance) NDIlib_recv_destroy(instance);
  }
};

const NDIlib_source_t* findSource(const NDIlib_source_t* sources, uint32_t count,
                                  const std::string& name) {
  for (uint32_t i = 0; i < count; ++i)
    if (sources[i].p_ndi_name && name == sources[i].p_ndi_name) return &sources[i];
  for (uint32_t i = 0; i < count; ++i)
    if (sources[i].p_ndi_name && std::string(sources[i].p_ndi_name).find(name) != std::string::npos)
      return &sources[i];
  return nullptr;
}

// NDI timestamps are 100 ns ticks, the same unit as Frame::timestamp.
int64_t ticks(int64_t timestamp) {
  return timestamp == NDIlib_recv_timestamp_undefined ? 0 : timestamp;
}

}  // namespace

NdiSource::NdiSource(std::string name, FrameSink onFrame, AudioSink onAudio, StateSink onState)
    : name_(std::move(name)),
      onFrame_(std::move(onFrame)),
      onAudio_(std::move(onAudio)),
      onState_(std::move(onState)) {}

NdiSource::~NdiSource() { stopImpl(false); }  // no state callback from a destructor

bool NdiSource::running() const {
  std::lock_guard l(mu_);
  return thread_.joinable();
}

void NdiSource::start() {
  std::lock_guard lifecycle(lifecycle_);
  {
    std::lock_guard l(mu_);
    if (thread_.joinable()) return;
    stop_.store(false);
  }
  if (onState_) onState_(SignalState::Connecting, std::nullopt);

  std::thread t;
  try {
    t = std::thread([this] { run(); });
  } catch (const std::exception& e) {
    // Never let a spawn failure escape into the graph's demand path.
    if (onState_) onState_(SignalState::Offline, ErrorInfo{"ndi-thread", e.what()});
    return;
  }
  {
    std::lock_guard l(mu_);
    thread_ = std::move(t);
  }
}

void NdiSource::stop() { stopImpl(true); }

void NdiSource::stopImpl(bool notify) {
  std::lock_guard lifecycle(lifecycle_);
  std::thread t;
  {
    std::lock_guard l(mu_);
    stop_.store(true);
    t = std::move(thread_);
  }
  if (t.joinable()) t.join();  // the worker destroys find/recv on its way out
  if (notify && onState_) onState_(SignalState::Ready, std::nullopt);
}

void NdiSource::setTally(bool program, bool preview) {
  tallyProgram_.store(program);
  tallyPreview_.store(preview);
  tallyDirty_.store(true);
}

void NdiSource::run() {
  if (!ensureNdi()) {
    if (onState_)
      onState_(SignalState::Offline, ErrorInfo{"ndi-failed", "the NDI runtime could not start"});
    return;
  }

  Finder finder;
  const NDIlib_find_create_t findSettings;  // show_local_sources = true, all groups
  finder.instance = NDIlib_find_create_v2(&findSettings);
  if (!finder.instance) {
    if (onState_)
      onState_(SignalState::Offline, ErrorInfo{"ndi-failed", "the NDI finder could not start"});
    return;
  }

  Receiver receiver;
  bool live = false;
  bool formatReported = false;  // one ndi-format report per session, not per frame
  bool errorReported = false;
  bool connectReported = false;  // one report per failed connect, not per find iteration
  auto last = std::chrono::steady_clock::now();

  try {
    while (!stop_.load()) {
      if (!receiver.instance) {
        NDIlib_find_wait_for_sources(finder.instance, kFindTimeoutMs);
        uint32_t count = 0;
        const NDIlib_source_t* sources =
            NDIlib_find_get_current_sources(finder.instance, &count);
        const NDIlib_source_t* match = findSource(sources, count, name_);
        if (!match) continue;  // senders come and go; keep looking
        NDIlib_recv_create_v3_t settings;
        settings.source_to_connect_to = *match;
        settings.color_format = NDIlib_recv_color_format_fastest;
        settings.bandwidth = NDIlib_recv_bandwidth_highest;
        settings.allow_video_fields = false;
        settings.p_ndi_recv_name = "OMT Bridge";
        receiver.instance = NDIlib_recv_create_v3(&settings);
        if (receiver.instance) {
          connectReported = false;  // a later failure is a new episode
        } else if (!connectReported) {
          connectReported = true;
          if (onState_)
            onState_(SignalState::Offline,
                     ErrorInfo{"ndi-failed", "cannot connect to NDI source \"" + name_ + "\""});
        }
        continue;
      }

      if (tallyDirty_.exchange(false)) {
        NDIlib_tally_t tally;
        tally.on_program = tallyProgram_.load();
        tally.on_preview = tallyPreview_.load();
        NDIlib_recv_set_tally(receiver.instance, &tally);
      }

      NDIlib_video_frame_v2_t v;
      NDIlib_audio_frame_v3_t a;
      switch (NDIlib_recv_capture_v3(receiver.instance, &v, &a, nullptr, kCaptureTimeoutMs)) {
        case NDIlib_frame_type_video: {
          errorReported = false;
          const bool uyva = v.FourCC == NDIlib_FourCC_type_UYVA;
          const int64_t stride =
              v.line_stride_in_bytes ? v.line_stride_in_bytes : static_cast<int64_t>(v.xres) * 2;
          // UYVA is a UYVY plane then a width*height alpha plane. A negative stride is
          // bottom-up; this bridge has no flip, so report it rather than guess.
          const bool take = (v.FourCC == NDIlib_FourCC_type_UYVY || uyva) && stride > 0 &&
                            v.p_data && v.xres >= 2 && v.yres >= 2 && v.xres % 2 == 0 &&
                            stride >= static_cast<int64_t>(v.xres) * 2;
          std::shared_ptr<Frame> f;
          if (!take) {
            live = false;  // ndi-format covers this dropout; no second Offline from the timeout
            if (!formatReported) {
              formatReported = true;
              if (onState_)
                onState_(SignalState::Offline,
                         ErrorInfo{"ndi-format", "the NDI source sent unsupported video"});
            }
          } else {
            try {
              f = std::make_shared<Frame>();
              f->format = VideoFormat{v.xres, v.yres, v.frame_rate_N, v.frame_rate_D, uyva};
              f->premultiplied = false;  // NDI is straight
              f->timestamp = ticks(v.timestamp);
              f->uyvy.resize(f->lumaBytes() + f->alphaBytes());
              const size_t rowBytes = static_cast<size_t>(v.xres) * 2u;
              for (int row = 0; row < v.yres; ++row)
                std::memcpy(f->uyvy.data() + static_cast<size_t>(row) * rowBytes,
                            v.p_data + static_cast<size_t>(row) * static_cast<size_t>(stride),
                            rowBytes);
              if (uyva)
                std::memcpy(f->uyvy.data() + f->alphaOffset(),
                            v.p_data + static_cast<size_t>(stride) * static_cast<size_t>(v.yres),
                            static_cast<size_t>(v.xres) * static_cast<size_t>(v.yres));
            } catch (...) {
              NDIlib_recv_free_video_v2(receiver.instance, &v);
              throw;
            }
          }
          NDIlib_recv_free_video_v2(receiver.instance, &v);
          if (!f || !f->valid()) continue;  // never publish a frame the hub rejects
          last = std::chrono::steady_clock::now();  // liveness follows usable frames
          if (!live) {
            live = true;
            if (onState_) onState_(SignalState::Live, std::nullopt);
          }
          if (onFrame_) onFrame_(std::move(f));
          break;
        }
        case NDIlib_frame_type_audio: {
          errorReported = false;
          std::shared_ptr<AudioFrame> out;
          const int64_t channelBytes =
              static_cast<int64_t>(a.no_samples) * static_cast<int64_t>(sizeof(float));
          if (a.FourCC == NDIlib_FourCC_audio_type_FLTP && a.p_data && a.sample_rate > 0 &&
              a.no_channels > 0 && a.no_samples > 0 &&
              a.channel_stride_in_bytes >= channelBytes) {
            try {
              out = std::make_shared<AudioFrame>();
              out->format = AudioFormat{a.sample_rate, a.no_channels};
              out->samples = a.no_samples;
              out->timestamp = ticks(a.timestamp);
              const size_t perChannel = static_cast<size_t>(a.no_samples);
              out->planar.resize(perChannel * static_cast<size_t>(a.no_channels));
              for (int c = 0; c < a.no_channels; ++c)
                std::memcpy(
                    out->planar.data() + static_cast<size_t>(c) * perChannel,
                    a.p_data +
                        static_cast<size_t>(c) * static_cast<size_t>(a.channel_stride_in_bytes),
                    perChannel * sizeof(float));
            } catch (...) {
              NDIlib_recv_free_audio_v3(receiver.instance, &a);
              throw;
            }
          }
          NDIlib_recv_free_audio_v3(receiver.instance, &a);
          if (!out || !out->valid()) break;
          if (onAudio_) {
            try {
              onAudio_(std::move(out));
            } catch (...) {
              // One bad packet must not end audio for the rest of the session.
            }
          }
          break;
        }
        case NDIlib_frame_type_error:
          live = false;  // already reported below; do not repeat as a liveness timeout
          if (!errorReported) {
            errorReported = true;
            if (onState_)
              onState_(SignalState::Offline, ErrorInfo{"ndi-failed", "the NDI source failed"});
          }
          break;
        default:
          break;
      }

      if (live && std::chrono::steady_clock::now() - last > kOfflineAfter) {
        live = false;  // was live, no video
        if (onState_) onState_(SignalState::Offline, std::nullopt);
      }
    }
  } catch (const std::exception& e) {
    if (onState_) onState_(SignalState::Offline, ErrorInfo{"ndi-receive", e.what()});
  }
}

}  // namespace ob