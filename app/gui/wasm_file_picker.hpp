#pragma once

namespace sigui {

// Opens the browser's file chooser (a real <input type="file"> restricted to
// .wav / .iq / .sigmf-data) and, when the user picks a file, passes its bytes and name to the app
// through the bridge's Embind entry point (Module.sigAppOpenFile). Must be called
// from within a user gesture (a click), as browsers require for file choosers.
void pick_file_in_browser();

}  // namespace sigui
