#pragma once
// One NDI output (design 8, 9.1). NDI is straight alpha only, so a premultiplied frame is
// converted here and nowhere else.
#include <memory>
#include <mutex>
#include <string>

#include "core/audio.h"
#include "core/bridge_model.h"
#include "core/frame.h"
#include "engine/output_row.h"

namespace ob {

struct NdiSenderHolder;  // hides the SDK from this header

class NdiSink : public Sink {
 public:
  NdiSink();
  ~NdiSink() override;
  NdiSink(const NdiSink&) = delete;
  NdiSink& operator=(const NdiSink&) = delete;

  void configure(const Output& output, const std::string& advertisedName) override;
  void deliver(const std::shared_ptr<const Frame>& frame) override;
  void deliverAudio(const std::shared_ptr<const AudioFrame>& frame) override;
  std::string note() const override;
  Tally tally() const override;
  SinkStats stats() const override;

 protected:
  // Called with mu_ held; overrides must not call a public method of NdiSink.
  virtual void send(const Frame& frame, bool alpha);
  virtual void openLocked();

 private:
  mutable std::mutex mu_;
  Output config_;
  std::string name_;
  std::string openNote_;
  Frame scratch_;  // reused by the straight-alpha conversion; guarded by mu_
  std::unique_ptr<NdiSenderHolder> sender_;
};

}  // namespace ob