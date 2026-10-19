#!/usr/bin/env bash
# Provision outside Ninja; dependency compilation itself is --frozen --offline.
set -euo pipefail
export PATH="${HOME}/.cargo/bin:${PATH}"
if ! command -v rustup >/dev/null; then
  rustup_dir=$(mktemp -d)
  trap 'rm -rf "$rustup_dir"' EXIT
  case "$(uname -m)" in x86_64) host=x86_64-unknown-linux-gnu;; aarch64) host=aarch64-unknown-linux-gnu;; *) exit 1;; esac
  url="https://static.rust-lang.org/rustup/dist/${host}/rustup-init"
  curl --fail --location --retry 3 "$url" -o "$rustup_dir/rustup-init"
  curl --fail --location --retry 3 "$url.sha256" -o "$rustup_dir/rustup-init.sha256"
  (cd "$rustup_dir" && sha256sum --check rustup-init.sha256)
  chmod +x "$rustup_dir/rustup-init"
  "$rustup_dir/rustup-init" -y --profile minimal --default-toolchain none --no-modify-path
fi
rustup toolchain install 1.96.1 --profile minimal --no-self-update
rustup target add --toolchain 1.96.1 aarch64-unknown-linux-ohos
