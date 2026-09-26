#pragma once

// Turns a caught exception into the message a user should see. Portable C++ (no
// Emscripten), so it is unit-tested natively.

#include <exception>
#include <new>
#include <string>

namespace sigbridge {

// std::bad_alloc's what() is "std::bad_alloc": true but useless to someone who just
// dropped a very large capture on the page. Everything else keeps its own message.
inline std::string describe_exception(const std::exception& e) {
    if (dynamic_cast<const std::bad_alloc*>(&e) != nullptr) {
        return "out of memory: this capture is too large for the browser to hold while analysing it. "
               "Try a shorter capture.";
    }
    return e.what();
}

}  // namespace sigbridge
