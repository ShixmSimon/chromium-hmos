#!/usr/bin/env python3
"""Classify probe color patches in recorded frames, not physical scanout.

Requires ffmpeg/ffprobe. Coordinates refer to the encoded video. Inspect the
transition images as well; a sampled patch cannot establish a universal atomic
presentation contract. Unknown colors are preserved, never treated as a pass.
"""
import argparse
import collections
import json
import subprocess
from pathlib import Path

PALETTE = {
    "blue": (0, 102, 255), "yellow": (255, 221, 0),
    "green": (0, 204, 34), "magenta": (221, 0, 221), "red": (255, 0, 0),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", type=Path)
    parser.add_argument("--point", action="append", required=True,
                        help="NAME:X:Y, center of an 8x8 encoded-pixel patch")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    probe = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames",
        "-show_streams", "-show_entries",
        "frame=best_effort_timestamp_time:stream=width,height", "-of", "json", str(args.video),
    ]))
    times = [float(f["best_effort_timestamp_time"]) for f in probe["frames"]]
    if not times:
        parser.error("recording contains no frames")
    width, height = (probe["streams"][0][key] for key in ("width", "height"))
    samples = {}
    points = {}
    for point in args.point:
        name, x, y = point.split(":")
        x, y = int(x), int(y)
        if name in points or not (4 <= x <= width - 4 and 4 <= y <= height - 4):
            parser.error("duplicate name or patch outside recording")
        points[name] = [x, y]
        raw = subprocess.check_output([
            "ffmpeg", "-v", "fatal", "-i", str(args.video), "-vf",
            f"crop=8:8:{x-4}:{y-4},scale=1:1:flags=area",
            "-fps_mode", "passthrough", "-pix_fmt", "rgb24", "-f", "rawvideo", "-",
        ])
        if len(raw) != len(times) * 3:
            raise RuntimeError("decoded patch count differs from timestamp count")
        samples[name] = []
        for offset in range(0, len(raw), 3):
            rgb = tuple(raw[offset:offset+3])
            distances = {key: sum((a-b)**2 for a, b in zip(rgb, color))
                         for key, color in PALETTE.items()}
            color = min(distances, key=distances.get)
            samples[name].append({"rgb": rgb,
                                  "color": color if distances[color] < 5000 else "other"})
    frames, transitions, counts = [], [], collections.Counter()
    previous = None
    for i, pts in enumerate(times):
        patches = {name: values[i] for name, values in samples.items()}
        state = tuple(patches[name]["color"] for name in points)
        row = {"frame": i, "pts": pts, "patches": patches}
        frames.append(row)
        counts[state] += 1
        if state != previous:
            transitions.append({**row, "previous_frame_gap_ms":
                                (pts-times[i-1])*1000 if i else None})
            previous = state
    summary = {"video": str(args.video), "dimensions": [width, height],
               "points": points, "frame_count": len(frames),
               "states": [{"colors": state, "count": count} for state, count in counts.items()],
               "transitions": transitions,
               "scope": "Recorded patch samples only; not physical presentation timestamps."}
    args.output.write_text(json.dumps({**summary, "frames": frames}, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
