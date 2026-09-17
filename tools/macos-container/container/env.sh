# Shared environment for chromium-hmos build steps. Keeps every cache on /work.
export HOME=/work/home
export PATH="/work/tools/depot_tools:$PATH"
export DEPOT_TOOLS_METRICS=0
export VPYTHON_VIRTUALENV_ROOT="$HOME/.vpython-root"
export CIPD_CACHE_DIR="$HOME/.cipd-cache"
export XDG_CACHE_HOME="$HOME/.cache"
export PYTHONDONTWRITEBYTECODE=1

readonly CHROMIUM_REVISION=f405107495a07cb1bfcf687d4af8d91117098db6
readonly CHROMIUM_ROOT=/work/chromium
readonly CHROMIUM_SRC="$CHROMIUM_ROOT/src"
readonly ADAPTER_ROOT=/work/tools/chromium-hmos

log() {
  echo "[$(date '+%F %T')] $*"
}
