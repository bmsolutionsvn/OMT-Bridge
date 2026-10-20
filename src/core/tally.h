#pragma once
// Downstream tally, as design 12.3 defines it: an output is on air when something downstream
// has it on air. The SourceHub ORs it across the outputs it feeds.
namespace ob {

struct Tally {
  bool program = false;
  bool preview = false;
  bool operator==(const Tally&) const = default;
};

}  // namespace ob
