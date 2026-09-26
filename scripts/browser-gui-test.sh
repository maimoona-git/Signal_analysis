#!/usr/bin/env bash
# The browser app end to end in real Chrome (tests/gui/browser-gui.test.mjs): builds
# nothing, so run scripts/build-wasm.sh first. Stages the web app, then drives it with
# mouse and keyboard through a real file chooser. Screenshots land in
# build-wasm/gui-screenshots/.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
"$REPO_DIR/scripts/stage-web.sh" >/dev/null
exec node --test "$REPO_DIR/tests/gui/browser-gui.test.mjs"
