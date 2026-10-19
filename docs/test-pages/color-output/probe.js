'use strict';
let report = {};
const mode = document.querySelector('#mode');
const show = () => document.querySelector('#report').textContent = JSON.stringify(report, null, 2);
function run() {
  const previous = document.querySelector('canvas');
  if (previous?._gl) previous._gl.getExtension('WEBGL_lose_context')?.loseContext();
  const c = document.createElement('canvas'); c.width = 600; c.height = 240;
  document.querySelector('#host').replaceChildren(c);
  report = {time: new Date().toISOString(), mode: mode.value, userAgent: navigator.userAgent,
    p3: matchMedia('(color-gamut: p3)').matches, hdr: matchMedia('(dynamic-range: high)').matches};
  c.addEventListener('webglcontextlost', e => { e.preventDefault(); if (c.isConnected) { report.contextLost = true; show(); } });
  try {
    const p3 = mode.value.includes('p3');
    const colorSpace = p3 ? 'display-p3' : 'srgb';
    if (mode.value.startsWith('2d')) {
      const hdr = mode.value.endsWith('hdr');
      const ctx = c.getContext('2d', {alpha:false, colorSpace, colorType: hdr ? 'float16' : 'unorm8', toneMapping:{mode:hdr?'extended':'standard'}});
      if (!ctx) throw Error('2D context unavailable');
      if (hdr) {
        report.hdrConfigurationApi = typeof c.configureHighDynamicRange;
        if (typeof c.configureHighDynamicRange !== 'function') throw Error('configureHighDynamicRange unavailable');
        c.configureHighDynamicRange({mode:'extended'});
        report.hdrConfigured = true;
      }
      report.attributes = ctx.getContextAttributes();
      ctx.fillStyle = p3 ? 'color(display-p3 0 1 0)' : '#00ff00'; ctx.fillRect(0,0,300,240);
      ctx.fillStyle = hdr ? 'color(display-p3 2 0 0)' : (p3 ? 'color(display-p3 1 0 0)' : '#ff0000'); ctx.fillRect(300,0,300,240);
      report.leftPixel = Array.from(ctx.getImageData(150,120,1,1).data);
    } else {
      const float = mode.value.endsWith('float');
      // Chromium requires an alpha channel for drawingBufferStorage().
      const gl = c.getContext('webgl2', {alpha:float, preserveDrawingBuffer:true, antialias:false});
      if (!gl) throw Error('WebGL2 context unavailable');
      c._gl = gl; report.attributes = gl.getContextAttributes();
      gl.drawingBufferColorSpace = colorSpace;
      if (float) {
        report.floatExtension = !!gl.getExtension('EXT_color_buffer_float');
        if (typeof gl.drawingBufferStorage !== 'function') throw Error('drawingBufferStorage unavailable');
        gl.drawingBufferStorage(gl.RGBA16F,600,240);
        if (mode.value.includes('hdr')) {
          if (typeof c.configureHighDynamicRange !== 'function') throw Error('configureHighDynamicRange unavailable');
          c.configureHighDynamicRange({mode:'extended'});
          report.hdrConfigured = true;
        }
      }
      report.colorSpace = gl.drawingBufferColorSpace;
      report.format = gl.drawingBufferFormat;
      report.setupError = gl.getError();
      gl.enable(gl.SCISSOR_TEST);
      gl.scissor(0,0,300,240); gl.clearColor(0,1,0,1); gl.clear(gl.COLOR_BUFFER_BIT);
      gl.scissor(300,0,300,240); gl.clearColor(float?2:1,0,0,1); gl.clear(gl.COLOR_BUFFER_BIT);
      report.drawError = gl.getError();
      const pixel = float ? new Float32Array(4) : new Uint8Array(4);
      gl.readPixels(150,120,1,1,gl.RGBA,float?gl.FLOAT:gl.UNSIGNED_BYTE,pixel);
      report.leftPixel = Array.from(pixel); report.readError = gl.getError();
      report.contextLost = gl.isContextLost();
    }
  } catch (e) { report.error = String(e); }
  show();
}
document.querySelector('#run').onclick = run;
document.querySelector('#save').onclick = () => {
  const a = document.createElement('a'); const url = URL.createObjectURL(new Blob([JSON.stringify(report,null,2)],{type:'application/json'}));
  a.href=url; a.download='color-output.json'; a.click(); setTimeout(()=>URL.revokeObjectURL(url),1000);
};
run();
