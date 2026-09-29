#include "wasm_file_picker.hpp"

#include <emscripten.h>

namespace sigui {

EM_JS(void, sig_pick_file_js, (), {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.wav,.iq,.IQ,.sigmf-data';
    input.style.display = 'none';
    input.addEventListener('change', async () => {
        const file = input.files && input.files[0];
        input.remove();
        if (!file) return;
        try {
            // Same entry point the drag-and-drop path uses: bytes into the WASM module.
            Module['sigAppOpenFile'](new Uint8Array(await file.arrayBuffer()), file.name);
        } catch (e) {
            console.error('could not load the chosen file', e);
        }
    });
    input.addEventListener('cancel', () => input.remove());
    document.body.appendChild(input);
    input.click();
});

void pick_file_in_browser() { sig_pick_file_js(); }

}  // namespace sigui
