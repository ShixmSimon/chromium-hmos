#!/usr/bin/env python3
"""Generate owned, deterministic-input video fixtures; verify decoded samples.

Requires ffmpeg (libx264/libx265/libvpx-vp9) and ffprobe, Python stdlib only.
Encoded bytes may differ across encoder versions; manifest records actual hashes.
"""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
WIDTH, HEIGHT, FPS, FRAMES = 1280, 720, 30, 180
NITS = [0, 0.1, 1, 10, 100, 203, 400, 1000]
MASTER = "G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(10000000,1)"
# Top 640 lines have eight equal patches, bottom 80 a moving white square.
MAX_FALL = math.ceil(sum(NITS) / 8 * 640 / HEIGHT + 1000 * 80 * 80 / (WIDTH * HEIGHT))


def code(nits, hdr, bits):
    if hdr:
        p = (nits / 10000) ** (2610 / 16384)
        signal = ((3424 / 4096 + 2413 / 128 * p) / (1 + 2392 / 128 * p)) ** (2523 / 32)
    else:
        linear = min(nits / 100, 1)
        signal = 4.5 * linear if linear < 0.018 else 1.099 * linear ** 0.45 - 0.099
    return round((16 + 219 * signal) * (1 << (bits - 8)))


def pack(values, bits):
    if bits == 8:
        return bytes(values)
    result = array('H', values)
    if sys.byteorder != 'little':
        result.byteswap()
    return result.tobytes()


def frames(hdr, bits):
    row = pack([code(n, hdr, bits) for n in NITS for _ in range(WIDTH // 8)], bits)
    top = row * 640
    black = pack([code(0, hdr, bits)] * WIDTH, bits)
    white = pack([code(1000 if hdr else 100, hdr, bits)] * 80, bits)
    chroma = pack([128 << (bits - 8)] * (WIDTH * HEIGHT // 2), bits)
    step = 1 if bits == 8 else 2
    for frame in range(FRAMES):
        x = (frame * 8) % (WIDTH - 80 + 1)
        moving = black[:x * step] + white + black[(x + 80) * step:]
        yield top + moving * 80 + chroma


SPECS = [
    ('sdr-avc', 'SDR / AVC / 8-bit', 'libx264', 8, False, 'mp4'),
    ('sdr-hevc10', 'SDR / HEVC / 10-bit', 'libx265', 10, False, 'mp4'),
    ('hdr10-hevc', 'HDR10 / HEVC / PQ / 10-bit', 'libx265', 10, True, 'mp4'),
    ('pq-vp9', 'PQ / VP9 profile 2 / 10-bit (no static HDR metadata)', 'libvpx-vp9', 10, True, 'webm'),
]


def probe(path):
    return json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-read_intervals', '%+#1',
        '-show_streams', '-show_frames', '-of', 'json', str(path)]))


def verify(path, spec):
    _, _, encoder, bits, hdr, _ = spec
    info = probe(path)
    stream = info['streams'][0]
    expected = {'width': WIDTH, 'height': HEIGHT, 'pix_fmt': 'yuv420p' if bits == 8 else 'yuv420p10le',
                'color_range': 'tv', 'color_transfer': 'smpte2084' if hdr else 'bt709',
                'color_primaries': 'bt2020' if hdr else 'bt709',
                'color_space': 'bt2020nc' if hdr else 'bt709',
                'codec_name': {'libx264': 'h264', 'libx265': 'hevc', 'libvpx-vp9': 'vp9'}[encoder]}
    for key, value in expected.items():
        if stream.get(key) != value:
            raise RuntimeError(f'{path.name}: {key}: {stream.get(key)!r} != {value!r}')
    side = info['frames'][0].get('side_data_list', [])
    mastering = [s for s in side if s['side_data_type'] == 'Mastering display metadata']
    light = [s for s in side if s['side_data_type'] == 'Content light level metadata']
    if hdr and encoder == 'libx265':
        if not mastering or not light or light[0]['max_content'] != 1000 or light[0]['max_average'] != MAX_FALL:
            raise RuntimeError(f'Missing/incorrect HDR10 SEI: {side}')
    elif mastering or light:
        raise RuntimeError(f'Unexpected static HDR metadata: {side}')
    raw = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(path), '-frames:v', '1',
                                   '-pix_fmt', expected['pix_fmt'], '-f', 'rawvideo', 'pipe:1'])
    samples = []
    for i, nits in enumerate(NITS):
        step = 1 if bits == 8 else 2
        offset = (320 * WIDTH + i * 160 + 80) * step
        value = int.from_bytes(raw[offset:offset + step], 'little')
        target = code(nits, hdr, bits)
        if abs(value - target) > 4:
            raise RuntimeError(f'{path.name}: patch {i} decoded {value}, expected {target}')
        samples.append({'nominal_nits': nits if hdr else min(nits, 100), 'expected_y': target, 'decoded_y': value})
    # Decode the entire clip: corrupt/truncated final GOPs must fail too.
    subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', str(path), '-f', 'null', '-'], check=True)
    count = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
        '-count_frames', '-show_entries', 'stream=nb_read_frames', '-of', 'json', str(path)]))
    if int(count['streams'][0]['nb_read_frames']) != FRAMES:
        raise RuntimeError(f'{path.name}: wrong frame count')
    return {'stream': stream, 'first_frame_side_data': side, 'patch_samples': samples,
            'decoded_frames': FRAMES, 'full_decode': 'passed'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'docs/test-pages/video-overlay-hdr/media')
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest_path = args.output / 'manifest.json'
    previous = json.loads(manifest_path.read_text()) if args.verify_only else None
    if not args.verify_only:
        manifest_path.unlink(missing_ok=True)  # Never serve a stale success manifest after a failed generation.
    manifest = {'schema': 1, 'width': WIDTH, 'height': HEIGHT, 'fps': FPS, 'frames': FRAMES,
                'duration_seconds': FRAMES / FPS, 'source': 'Generated grayscale PQ/BT709 patterns; no external media',
                'ffmpeg_version': subprocess.check_output(['ffmpeg', '-version'], text=True).splitlines()[0],
                'generator_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'clips': []}
    for spec in SPECS:
        ident, title, encoder, bits, hdr, extension = spec
        path = args.output / f'{ident}.{extension}'
        color_args = ['-color_range', 'tv', '-color_primaries', 'bt2020' if hdr else 'bt709',
                      '-color_trc', 'smpte2084' if hdr else 'bt709', '-colorspace', 'bt2020nc' if hdr else 'bt709']
        command = ['ffmpeg', '-hide_banner', '-loglevel', 'warning', '-y', '-f', 'rawvideo',
                   '-pixel_format', 'yuv420p' if bits == 8 else 'yuv420p10le', '-video_size', f'{WIDTH}x{HEIGHT}',
                   '-framerate', str(FPS)] + color_args + ['-i', 'pipe:0', '-an', '-c:v', encoder, '-threads', '4'] + color_args
        if encoder == 'libx265':
            params = 'pools=4:frame-threads=2:log-level=error:keyint=30:min-keyint=30:scenecut=0'
            if hdr:
                params += f':hdr10=1:master-display={MASTER}:max-cll=1000,{MAX_FALL}'
            command += ['-preset', 'fast', '-crf', '12', '-x265-params', params, '-tag:v', 'hvc1']
        elif encoder == 'libx264':
            command += ['-preset', 'fast', '-crf', '12', '-g', '30', '-pix_fmt', 'yuv420p']
        else:
            command += ['-profile:v', '2', '-crf', '16', '-b:v', '0', '-deadline', 'good', '-cpu-used', '4', '-g', '30']
        if extension == 'mp4':
            command += ['-movflags', '+faststart']
        command.append(str(path))
        if not args.verify_only:
            print(f'Generating {path.name}', flush=True)
            with subprocess.Popen(command, stdin=subprocess.PIPE) as process:
                try:
                    for frame in frames(hdr, bits):
                        process.stdin.write(frame)
                finally:
                    process.stdin.close()
                if process.wait() != 0:
                    raise RuntimeError(f'Encoding failed: {path}')
        checked = verify(path, spec)
        if previous:
            recorded = next(c for c in previous['clips'] if c['id'] == ident)
            if recorded['sha256'] != hashlib.sha256(path.read_bytes()).hexdigest():
                raise RuntimeError(f'{path.name}: hash differs from manifest')
        manifest['clips'].append({'id': ident, 'title': title, 'file': path.name,
            'mime': 'video/mp4' if extension == 'mp4' else 'video/webm', 'hdr': hdr, 'bits': bits,
            'static_hdr_metadata': hdr and encoder == 'libx265',
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'bytes': path.stat().st_size,
            'encode_command': command, 'verification': checked})
        print(f'Verified {path.name}', flush=True)
    if not args.verify_only:
        temporary = args.output / 'manifest.json.tmp'
        temporary.write_text(json.dumps(manifest, indent=2) + '\n')
        temporary.replace(manifest_path)
    print(f'Ready: {manifest_path}')


if __name__ == '__main__':
    main()
