#pragma once

#include "main_window.hpp"

namespace sigui {

// Tells the Embind hooks (sigAppOpenFile, sigAppState) which window to drive.
void register_window_for_js(MainWindow* window);

}  // namespace sigui
