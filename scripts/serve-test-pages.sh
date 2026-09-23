#!/bin/bash
# Serves docs/test-pages to the phone as http://localhost:8080/.
#
# localhost is a secure context, which notifications, WebAuthn and the Shape
# Detection API all require; about:blank and data: URLs are not. `hdc rport`
# makes the phone's port 8080 reach this machine's.
#
# Usage: scripts/serve-test-pages.sh   (Ctrl-C to stop)
# Then open http://localhost:8080/hardware-apis.html in the browser on the phone.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
hdc="${HDC:-hdc}"
command -v "$hdc" >/dev/null 2>&1 ||
  hdc=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc
"$hdc" rport tcp:8080 tcp:8080
echo "Open http://localhost:8080/hardware-apis.html on the phone."
cd "$here/docs/test-pages"
exec python3 -m http.server 8080 --bind 127.0.0.1
