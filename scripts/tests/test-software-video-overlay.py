#!/usr/bin/env python3
"""Regress the real format guards and upload content IDs without Chromium."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def compile_run(source):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder)
        (path / 'test.cc').write_text(source)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall',
                        '-Wextra', '-Werror', str(path / 'test.cc'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class SoftwareVideoTest(unittest.TestCase):
    def test_sdr_upload_gate_excludes_hdr_alpha_and_unsupported_outputs(self):
        patch = (ROOT / 'patches/ohos-software-video-overlay.patch').read_text()
        added = '\n'.join(line[1:] for line in patch.splitlines()
                          if line.startswith('+') and not line.startswith('+++'))
        expression = re.search(r'ohos_software_overlay =\n(.*?);', added, re.S)[1]
        compile_run('''
#include <cassert>
#include <initializer_list>
namespace gfx {
struct ColorSpace {
  int id;
  static ColorSpace CreateSRGB() { return {1}; }
  static ColorSpace CreateREC709() { return {2}; }
  ColorSpace GetAsFullRangeRGB() const { return *this; }
  bool operator==(ColorSpace other) const { return id == other.id; }
};
}
bool IsYuvPlanar(int format) { return format != 3; }
bool IsOpaque(int format) { return format != 2; }
struct Caps {
  bool supports_ohos_video_native_buffers, supports_scanout_shared_images;
};
struct Provider {
  Caps caps;
  Provider* SharedImageInterface() { return this; }
  Caps GetCapabilities() { return caps; }
};
int main() {
  for (int color : {0, 1, 2, 3, 4}) { // invalid, sRGB, BT709, PQ, Display-P3
    for (int input_frame_format : {1, 2, 3}) { // opaque YUV, alpha YUV, RGB
      for (int bits_per_channel : {8, 10, 12}) {
        for (int caps = 0; caps < 4; ++caps) {
          for (bool cpu : {false, true}) {
            auto software_compositor = [cpu] { return cpu; };
            gfx::ColorSpace rgb_space{color};
            Provider provider{{bool(caps & 1), bool(caps & 2)}};
            Provider* context_provider_ = &provider;
            bool eligible = EXPRESSION;
            assert(eligible == (!cpu && caps == 3 && bits_per_channel == 8 &&
                   input_frame_format == 1 && (color == 1 || color == 2)));
          }
        }
      }
    }
  }
}
'''.replace('EXPRESSION', expression))

    def test_ordinary_yuv_does_not_clear_an_unset_external_sampler(self):
        patch = (ROOT / 'patches/ohos-software-video-overlay.patch').read_text()
        # Execute the replacement guard in each actual patch hunk. The stub
        # enforces SharedImageFormat's CHECK precondition on Clear, including
        # the I420-without-external-sampler input from the device crash.
        after = '\n'.join(line[1:] for line in patch.splitlines()
                          if line.startswith(('+', ' ')) and not line.startswith('+++'))
        guards = re.findall(r'if \((\w+)\.PrefersExternalSampler\(\)\)\n'
                            r'\s+\1\.ClearPrefersExternalSampler\(\);', after)
        self.assertCountEqual(guards, ['dawn_format', 'native_format'])
        for variable in guards:
            snippet = re.search(r'if \(' + variable + r'\.PrefersExternalSampler\(\)\)\n'
                                r'\s+' + variable + r'\.ClearPrefersExternalSampler\(\);', after)[0]
            compile_run('''
#include <cassert>
#include <initializer_list>
struct Format {
  bool multi, external;
  bool PrefersExternalSampler() const { return multi && external; }
  void ClearPrefersExternalSampler() {
    assert(multi && external);
    external = false;
  }
};
int main() {
  for (bool multi : {false, true}) {
    for (bool external : {false, true}) {
      Format FORMAT{multi, external};
      GUARD
      assert(FORMAT.external == (!multi && external));
    }
  }
}
'''.replace('FORMAT', variable).replace('GUARD', snippet))

    def test_uploaded_content_changes_only_after_a_fenced_write(self):
        source = (ROOT / 'overlay/ui/ozone/platform/ohos/ohos_native_pixmap.cc').read_text()
        functions = '\n'.join(function(source, signature) for signature in (
            'void AdvanceOhosSoftwareVideoFrame(', 'uint64_t GetOhosSoftwareVideoFrame('))
        compile_run('''
#include <atomic>
#include <cassert>
#include <cstdint>
namespace gfx { struct NativePixmap {}; }
struct OhosNativePixmap : gfx::NativePixmap {
  bool software_video_overlay = false;
  std::atomic<uint64_t> software_video_content{0};
};
namespace base {
uint64_t RandUint64() { static uint64_t counter = 0; return counter++; }
}
FUNCTIONS
int main() {
  OhosNativePixmap image;
  AdvanceOhosSoftwareVideoFrame(image, true);
  assert(GetOhosSoftwareVideoFrame(image) == 0); // Ordinary canvas isn't video.
  image.software_video_overlay = true;
  AdvanceOhosSoftwareVideoFrame(image, true);
  auto first = GetOhosSoftwareVideoFrame(image);
  assert(first != 0 && GetOhosSoftwareVideoFrame(image) == first); // Paused read.
  AdvanceOhosSoftwareVideoFrame(image, true);
  assert(GetOhosSoftwareVideoFrame(image) != first); // Recycled allocation.
  AdvanceOhosSoftwareVideoFrame(image, false);
  assert(GetOhosSoftwareVideoFrame(image) == 0); // Lost fence: no promotion.
}
'''.replace('FUNCTIONS', functions))


if __name__ == '__main__':
    unittest.main()
