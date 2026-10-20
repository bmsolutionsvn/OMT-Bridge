#include "engine/ndi_runtime.h"

#include <Processing.NDI.Lib.h>

namespace ob {

bool ensureNdi() {
  static const bool ok = NDIlib_initialize();
  return ok;
}

}  // namespace ob
