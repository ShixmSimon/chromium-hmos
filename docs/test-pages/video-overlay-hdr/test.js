'use strict';
const $ = id => document.getElementById(id);
const video = $('video');
let manifest, activeClip = null, generation = 0, run = null, frames = 0, lastFrame = null;
const events = [];
const started = new Date().toISOString();

function log(type, detail = {}) {
  events.push({wallTime: new Date().toISOString(), monotonicMs: performance.now(),
    generation, clip: activeClip?.id ?? null, type, mediaTime: video.currentTime, ...detail});
  if (events.length > 300) events.shift();
  $('events').textContent = events.map(e => JSON.stringify(e)).join('\n');
}
function status(message) { $('status').textContent = message; }
function cancel() {
  generation++;
  run?.abort();
  run = null;
}
function resetMedia() {
  video.pause();
  video.removeAttribute('src');
  video.load();
  activeClip = null;
  frames = 0;
  lastFrame = null;
}
function waitForMedia(signal) {
  return new Promise((resolve, reject) => {
    const cleanup = () => {
      clearTimeout(timer);
      video.removeEventListener('loadeddata', ready);
      video.removeEventListener('error', error);
      signal.removeEventListener('abort', abort);
    };
    const ready = () => { cleanup(); resolve(); };
    const error = () => { cleanup(); reject(new Error(`media error ${video.error?.code}: ${video.error?.message}`)); };
    const abort = () => { cleanup(); reject(new DOMException('Cancelled', 'AbortError')); };
    const timer = setTimeout(() => { cleanup(); reject(new Error('媒体加载超时（20 秒）')); }, 20000);
    video.addEventListener('loadeddata', ready);
    video.addEventListener('error', error);
    signal.addEventListener('abort', abort, {once: true});
    if (signal.aborted) abort();
  });
}
async function loadClip(clip, signal) {
  resetMedia();
  activeClip = clip;
  $('clip').value = clip.id;
  video.loop = $('loop').checked;
  status(`正在载入：${clip.title}`);
  const loaded = waitForMedia(signal);
  video.src = new URL(`media/${clip.file}`, location.href).href;
  log('load-request', {sha256: clip.sha256});
  await loaded;
  if (signal.aborted) throw new DOMException('Cancelled', 'AbortError');
  await video.play();
  if (signal.aborted) throw new DOMException('Cancelled', 'AbortError');
  status(`播放中：${clip.title}；独立图层 / HDR 出屏：尚未测量`);
}
function delay(ms, signal) {
  return new Promise((resolve, reject) => {
    const abort = () => { clearTimeout(timer); reject(new DOMException('Cancelled', 'AbortError')); };
    const timer = setTimeout(() => { signal.removeEventListener('abort', abort); resolve(); }, ms);
    signal.addEventListener('abort', abort, {once: true});
    if (signal.aborted) abort();
  });
}
async function start(sequence) {
  cancel();
  const controller = new AbortController();
  run = controller;
  try {
    for (const [index, id] of sequence.entries()) {
      await loadClip(manifest.clips.find(c => c.id === id), controller.signal);
      if (index < sequence.length - 1) await delay(6000, controller.signal);
    }
    if (sequence.length > 1) log('cycle-complete');
  } catch (error) {
    if (error.name !== 'AbortError' && !controller.signal.aborted) {
      log('failure', {message: String(error)});
      status(`测试中止：${error.message}`);
      resetMedia();
    }
  }
}
function snapshot() {
  const rect = video.getBoundingClientRect();
  return {schema: 1, started, exported: new Date().toISOString(), userAgent: navigator.userAgent,
    notes: $('notes').value, page: location.href, manifest, activeClip: activeClip?.id ?? null,
    reportedDynamicRangeHigh: matchMedia('(dynamic-range: high)').matches,
    reportedVideoDynamicRangeHigh: matchMedia('(video-dynamic-range: high)').matches,
    nativeEvidence: {decoderBufferFormat: 'not-measured', bufferColorAndMetadata: 'not-measured',
      independentVideoLayer: 'not-measured', displayHdr: 'not-measured', presentationTimestamp: 'not-measured'},
    media: {currentTime: video.currentTime, duration: Number.isFinite(video.duration) ? video.duration : null,
      width: video.videoWidth, height: video.videoHeight, paused: video.paused, readyState: video.readyState,
      error: video.error ? {code: video.error.code, message: video.error.message} : null,
      quality: video.getVideoPlaybackQuality?.().toJSON?.() ?? (() => {
        const q = video.getVideoPlaybackQuality?.();
        return q ? {totalVideoFrames: q.totalVideoFrames, droppedVideoFrames: q.droppedVideoFrames} : null;
      })(), observedFrameCallbacks: frames, lastFrame},
    layout: {rect: rect.toJSON(), fullscreen: !!document.fullscreenElement,
      fit: video.style.objectFit || 'contain', stageClass: $('stage').className,
      subtitle: !$('sub').hidden, danmaku: !$('dan').hidden, occlusion: !$('occluder').hidden},
    events: [...events]};
}
// Observation only: no canvas / VideoFrame readback that could change overlay eligibility.
if (video.requestVideoFrameCallback) {
  const observe = (now, metadata) => {
    frames++;
    lastFrame = {callbackMs: now, mediaTime: metadata.mediaTime,
      presentedFrames: metadata.presentedFrames, expectedDisplayTime: metadata.expectedDisplayTime};
    video.requestVideoFrameCallback(observe);
  };
  video.requestVideoFrameCallback(observe);
}
for (const type of ['loadedmetadata', 'loadeddata', 'playing', 'pause', 'waiting', 'stalled', 'seeking', 'seeked', 'ended', 'error', 'emptied']) {
  video.addEventListener(type, () => log(type, {readyState: video.readyState, errorCode: video.error?.code ?? null}));
}
document.addEventListener('fullscreenchange', () => log('fullscreenchange'));
document.addEventListener('visibilitychange', () => log('visibilitychange', {state: document.visibilityState}));
$('play').onclick = () => start([$('clip').value]);
$('cycle').onclick = () => start(['sdr-avc', 'hdr10-hevc', 'sdr-avc']);
$('stop').onclick = () => { cancel(); resetMedia(); log('stop'); status('已停止并释放媒体源'); };
$('pause').onclick = async () => {
  cancel();
  try { if (video.paused && activeClip) await video.play(); else video.pause(); }
  catch (error) { log('play-rejected', {message: String(error)}); }
};
$('seek').onclick = () => {
  cancel();
  if (Number.isFinite(video.duration) && video.duration > 0) video.currentTime = (video.currentTime + 2) % video.duration;
};
$('loop').onchange = () => { video.loop = $('loop').checked; log('loop', {enabled: video.loop}); };
$('fullscreen').onclick = async () => {
  try { if (document.fullscreenElement) await document.exitFullscreen(); else await $('stage').requestFullscreen(); }
  catch (error) { log('fullscreen-rejected', {message: String(error)}); }
};
for (const [control, target] of [['subtitle', 'sub'], ['danmaku', 'dan'], ['occlusion', 'occluder']]) {
  $(control).onchange = () => { $(target).hidden = !$(control).checked; log('layout', {control, enabled: $(control).checked}); };
}
for (const [control, name] of [['small', 'small'], ['transform', 'transformed']]) {
  $(control).onchange = () => { $('stage').classList.toggle(name, $(control).checked); log('layout', {control, enabled: $(control).checked}); };
}
$('fit').onchange = () => { video.style.objectFit = $('fit').value; log('fit', {value: $('fit').value}); };
$('export').onclick = () => {
  const url = URL.createObjectURL(new Blob([JSON.stringify(snapshot(), null, 2)], {type: 'application/json'}));
  const anchor = document.createElement('a');
  anchor.href = url; anchor.download = `lumie-video-hdr-${Date.now()}.json`; anchor.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
};
setInterval(() => { const data = snapshot(); $('stats').textContent = JSON.stringify({clip: data.activeClip,
  reportedDynamicRangeHigh: data.reportedDynamicRangeHigh, media: data.media, layout: data.layout}, null, 2); }, 1000);
fetch('media/manifest.json', {cache: 'no-store'}).then(response => {
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}).then(data => {
  if (data.schema !== 1 || !Array.isArray(data.clips) || !data.clips.length) throw new Error('素材清单格式错误');
  manifest = data;
  for (const clip of data.clips) {
    const option = document.createElement('option'); option.value = clip.id; option.textContent = clip.title;
    $('clip').append(option);
  }
  for (const id of ['clip', 'play', 'cycle']) $(id).disabled = false;
  status('素材已就绪，点击载入并播放。');
  log('ready');
}).catch(error => { status(`素材尚未就绪：${error.message}。请运行 generate_hdr_media.py。`); log('manifest-error', {message: String(error)}); });
window.videoOverlayTest = {snapshot};
