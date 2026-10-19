#pragma once
// One OMT or OMT-HX output (design 8, 9.3). Each row owns its own omthx::Sender, so a new output
// joins cleanly and a keyframe is forced on its first frame by the library.
#include <memory>
#include <mutex>
#include <string>

#include "core/bridge_model.h"
#include "core/frame.h"
#include "engine/output_row.h"

namespace ob {

struct OmtSenderHolder;  // hides omthx from this header

class OmtSink : public Sink {
 public:
  OmtSink();
  ~OmtSink() override;
  OmtSink(const OmtSink&) = delete;
  OmtSink& operator=(const OmtSink&) = delete;

  void configure(const Output& output, const std::string& advertisedName) override;
  void deliver(const std::shared_ptr<const Frame>& frame) override;
  std::string note() const override;

 protected:
  // Both virtuals are called with mu_ held. An override must not call any public method of
  // OmtSink: configure(), deliver() and note() all take mu_, so it would self-deadlock.
  // Overridden by the test to observe the decision without opening a sender.
  virtual void send(const VideoFormat& format, const uint8_t* uyvy, bool alpha,
                    bool premultiplied, int64_t timestamp);
  // Overridden by the test so configure() opens no network sender.
  virtual void openLocked();

 private:
  mutable std::mutex mu_;
  Output config_;
  std::string name_;
  std::string openNote_;   // NDI / open failure: outlives per-frame labels
  std::string frameNote_;  // this frame's decision
  std::unique_ptr<OmtSenderHolder> sender_;
};

}  // namespace ob
