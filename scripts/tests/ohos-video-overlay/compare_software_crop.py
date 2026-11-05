#!/usr/bin/env python3
"""Compare device screenshots captured by software_crop_validation.mjs.

Uses the asymmetric static fixture, excludes a two-pixel rasterization boundary,
and locates the green viewport marker independently in each screenshot. A
passing capture script alone does not establish pixel correctness.
"""
import argparse
import json
import math
from pathlib import Path
import numpy as np
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
args = parser.parse_args()
report = json.loads((args.directory / 'report.json').read_text())
results = []

def load(path):
    pixels = np.asarray(Image.open(path).convert('RGB')).astype(np.int16)
    # Marker is fixed at viewport (0,0), independent of browser chrome height.
    match = np.max(np.abs(pixels[:, :50] - [1, 254, 7]), axis=2) <= 2
    ys, xs = np.nonzero(match)
    if len(ys) < 25:
        raise RuntimeError(f'Viewport marker missing: {path}')
    return pixels, int(ys.min())

for case in report['cases']:
    name = case['name']
    if not all((args.directory / f'{name}-{mode}.png').exists()
               for mode in ('promoted', 'composited')):
        results.append(dict(name=name, passed=False, reason='Missing capture'))
        continue
    a, ay = load(args.directory / f'{name}-promoted.png')
    b, by = load(args.directory / f'{name}-composited.png')
    l, t, r, bottom = case['geometry']['roi']
    l, t, r, bottom = math.ceil(l)+2, math.ceil(t)+2, math.floor(r)-2, math.floor(bottom)-2
    if l >= r or t >= bottom:
        raise RuntimeError(f'Empty comparison region: {name}')
    x = a[t+ay:bottom+ay, l:r]
    y = b[t+by:bottom+by, l:r]
    if x.shape != y.shape or x.size == 0:
        raise RuntimeError(f'Invalid screenshot region: {name}')
    diff = np.abs(x-y)
    # Different RS/Skia sampling can round by one code value. Larger mismatch
    # or mean error exposes incorrect source position, scaling or clipping.
    mean = float(diff.mean())
    bad = float(np.mean(np.max(diff, axis=2) > 3))
    results.append(dict(name=name, roi=[l,t,r,bottom], max=int(diff.max()),
                        mean=mean, fraction_over_3=bad,
                        passed=mean <= 0.6 and bad <= 0.01))
output = dict(capture_passed=report.get('passed',False), cases=results,
              passed=bool(report.get('passed')) and len(results) == (8 if report.get('policy') == 'viewport-fallback' else 7) and
                     all(r['passed'] for r in results))
(args.directory / 'pixel-comparison.json').write_text(json.dumps(output,indent=2)+'\n')
print(json.dumps(output,indent=2))
raise SystemExit(0 if output['passed'] else 1)
