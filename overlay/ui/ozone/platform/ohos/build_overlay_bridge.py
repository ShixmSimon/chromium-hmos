#!/usr/bin/env python3
"""Build the RenderService adapter with the system NDK's __h C++ ABI."""
import argparse
import pathlib
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--sdk', required=True)
parser.add_argument('--out', required=True)
args = parser.parse_args()
sdk = pathlib.Path(args.sdk).resolve() / 'native'
source = pathlib.Path(__file__).resolve().parent
output = pathlib.Path(args.out).resolve()
output.parent.mkdir(parents=True, exist_ok=True)
sources = ['ohos_overlay_session.cc', 'ohos_rs_overlay_backend.cc',
           'ohos_overlay_bridge.cc', 'ohos_overlay_transport.cc', 'ohos_overlay_host.cc']
subprocess.run([str(sdk / 'llvm/bin/clang++'), '--target=aarch64-linux-ohos',
                '--sysroot=' + str(sdk / 'sysroot'), '-std=c++17', '-O2',
                '-fno-exceptions', '-fno-rtti', '-D_LIBCPP_ABI_NAMESPACE=__h',
                '-Wall', '-Wextra', '-Werror', '-fPIC', '-fvisibility=hidden', '-shared',
                *[str(source / name) for name in sources],
                '-lnative_buffer', '-lipc_capi', '-lace_ndk.z', '-luv', '-ldl',
                '-Wl,--no-undefined', '-Wl,-soname,libohos_video_overlay.so',
                '-o', str(output)], check=True)
