#include "LiveReload.h"

namespace LiveReload {
bool& EnabledFlag() {
    static bool enabled = true;
    return enabled;
}
}
