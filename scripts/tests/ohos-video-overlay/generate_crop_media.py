#!/usr/bin/env python3
"""Generate an owned asymmetric 640x360 SDR VP9 crop comparison fixture."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
# A two-dimensional pattern exposes stretching, translation and stale edge
# pixels. Identical frames permit pixel comparisons while playback continues.
w, h = 640, 360
frame = bytearray()
for y in range(h):
    for x in range(w):
        edge = x < 4 or y < 4 or x >= w-4 or y >= h-4
        frame.extend((255, 0, 255) if edge else
                     ((x // 40 * 31 + y // 30 * 17) % 256,
                      (y // 30 * 23 + x // 80 * 13) % 256,
                      (x // 20 * 11 + y // 60 * 47) % 256))
proc = subprocess.Popen(['ffmpeg', '-y', '-v', 'error', '-f', 'rawvideo',
                         '-pixel_format', 'rgb24', '-video_size', f'{w}x{h}',
                         '-framerate', '30', '-i', 'pipe:0', '-an', '-c:v',
                         'libvpx-vp9', '-pix_fmt', 'yuv420p', '-crf', '20',
                         '-b:v', '0', '-color_primaries', 'bt709', '-color_trc',
                         'bt709', '-colorspace', 'bt709', '-color_range', 'tv',
                         str(args.output)], stdin=subprocess.PIPE)
try:
    for _ in range(180):
        proc.stdin.write(frame)
finally:
    proc.stdin.close()
if proc.wait():
    raise SystemExit('ffmpeg fixture generation failed')
print(args.output)
