#include <doctest/doctest.h>

#include "engine/ndi_runtime.h"

#ifdef OB_WITH_NDI
TEST_CASE("the NDI runtime initializes when built with NDI") {
  CHECK(ob::ensureNdi());
  CHECK(ob::ensureNdi());  // idempotent: one process-wide init
}
#endif
