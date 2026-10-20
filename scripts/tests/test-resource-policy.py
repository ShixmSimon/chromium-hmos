#!/usr/bin/env python3
"""Host-only tests of the production resource classifier; no Chromium build."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
unit = r'''
#include <cassert>
#include "chrome/common/ohos/resource_policy.h"
using namespace chrome::ohos::resources;
int main() {
  assert(Classify("/signed", "video/mp4", "", 200, "GET") == "video");
  assert(Classify("/MOVIE.MP4", "application/octet-stream", "", 206, "GET") == "video");
  assert(Classify("/song", "Audio/MPEG", "", 200, "GET") == "audio");
  assert(Classify("/image", "image/avif", "", 200, "GET") == "image");
  assert(Classify("/playlist", "application/vnd.apple.mpegurl", "", 200, "GET") == "hls");
  assert(Classify("/list.M3U8", "text/plain", "", 200, "GET") == "hls");
  assert(Classify("/list", "application/dash+xml", "", 200, "GET") == "dash");
  assert(Classify("/list.mpd", "application/octet-stream", "", 200, "GET") == "dash");
  assert(Classify("/report", "application/pdf", "", 200, "GET") == "file");
  assert(Classify("/file", "application/zip", "attachment; filename=x.zip", 200, "GET") == "file");
  for (const auto* path : {"/video.mp4", "/list.m3u8", "/list.mpd"}) {
    assert(Classify(path, "text/html", "", 200, "GET").empty());
    assert(Classify(path, "application/json", "", 200, "GET").empty());
    for (int status : {204, 301, 304, 401, 403, 404, 500})
      assert(Classify(path, "video/mp4", "", status, "GET").empty());
    assert(Classify(path, "video/mp4", "", 200, "POST").empty());
    assert(Classify(path, "video/mp4", "", 200, "HEAD").empty());
  }
  assert(Classify("/segment.m4s", "video/mp4", "", 200, "GET").empty());
  assert(Classify("/segment.ts", "application/octet-stream", "", 200, "GET").empty());
  assert(Classify("/segment", "video/mp2t", "", 200, "GET").empty());
  assert(Classify("/x", "application/javascript", "", 200, "GET").empty());
  assert(TotalBytes(200, 987, "") == 987);
  assert(TotalBytes(200, -1, "") == -1);
  assert(TotalBytes(206, 10, "bytes 0-9/9999999999") == 9999999999LL);
  assert(TotalBytes(206, 10, "bytes 100-109/110") == 110);
  for (const auto* invalid : {"", "bytes 0-9/*", "bytes 0-9/9", "bytes 9-0/10",
       "bytes 0-9/10x", "bytes 0-9/99999999999999999999999", "bytes -1-9/10",
       "bytes 0-/10", "bytes 0-9/+10", "items 0-9/10"})
    assert(TotalBytes(206, 10, invalid) == -1);
}
'''
with tempfile.TemporaryDirectory(prefix='resource-policy-') as directory:
    source = Path(directory) / 'policy.cc'
    binary = Path(directory) / 'policy'
    source.write_text(unit)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra',
                    '-Werror', '-I' + str(root / 'overlay'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Resource MIME/status/method/fragment and partial-length policy tests passed.')
