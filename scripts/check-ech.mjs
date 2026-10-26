// Run against the OHOS browser's forwarded CDP endpoint, not desktop Chrome.
import { pathToFileURL } from 'node:url';

export function echEvidence(url, responses) {
  const hostname = new URL(url).hostname;
  const sameHost = responses.filter(r => {
    try { return new URL(r.url).hostname === hostname; } catch { return false; }
  });
  const last = sameHost.at(-1);
  if (!last) return { result: 'inconclusive', reason: 'No target-host main-document response', responses };
  const accepted = last.securityDetails?.encryptedClientHello;
  return {
    result: accepted === true ? 'accepted' : accepted === false ? 'not-accepted' : 'inconclusive',
    reason: accepted === undefined ? 'CDP did not expose ECH negotiation' : 'TLS securityDetails.encryptedClientHello',
    response: { url: last.url, status: last.status, protocol: last.protocol,
      tls: last.securityDetails?.protocol, encryptedClientHello: accepted },
    responses: responses.map(r => ({ url: r.url, status: r.status, protocol: r.protocol,
      encryptedClientHello: r.securityDetails?.encryptedClientHello }))
  };
}

async function main() {
  const url = process.argv[2], endpoint = process.argv[3] || 'http://127.0.0.1:9222';
  if (!url || new URL(url).protocol !== 'https:') throw Error('Usage: node scripts/check-ech.mjs <https-url> [CDP http endpoint]');
  const api = async path => {
    const r = await fetch(endpoint + path, { method: path.startsWith('/json/new?') ? 'PUT' : 'GET',
      signal: AbortSignal.timeout(10000) });
    if (!r.ok) throw Error(`CDP discovery HTTP ${r.status}`);
    return r.json();
  };
  const target = await api('/json/new?about:blank');
  const socket = new WebSocket(target.webSocketDebuggerUrl);
  const pending = new Map();
  let id = 0, finished;
  const loaded = new Promise(resolve => { finished = resolve; });
  const responses = [];
  let frameId;
  let failure = '';
  const call = (method, params = {}) => new Promise((resolve, reject) => {
    const request = ++id;
    const timer = setTimeout(() => { pending.delete(request); reject(Error(`CDP timeout: ${method}`)); }, 10000);
    pending.set(request, { resolve, reject, timer });
    socket.send(JSON.stringify({ id: request, method, params }));
  });
  socket.addEventListener('message', event => {
    const message = JSON.parse(event.data);
    if (message.id) {
      const waiter = pending.get(message.id);
      if (!waiter) return;
      pending.delete(message.id); clearTimeout(waiter.timer);
      if (message.error) waiter.reject(Error(JSON.stringify(message.error)));
      else waiter.resolve(message.result);
    } else if (message.method === 'Network.responseReceived' && message.params.type === 'Document' &&
      message.params.frameId === frameId) responses.push(message.params.response);
    else if (message.method === 'Page.loadEventFired') finished();
    else if (message.method === 'Network.loadingFailed' && message.params.type === 'Document' && !message.params.canceled) {
      failure = message.params.errorText; finished();
    }
  });
  let deadline;
  try {
    await new Promise((resolve, reject) => {
      socket.addEventListener('open', resolve, { once: true });
      socket.addEventListener('error', reject, { once: true });
      deadline = setTimeout(() => reject(Error('CDP connection timeout')), 10000);
    });
    clearTimeout(deadline);
    await call('Page.enable'); await call('Network.enable');
    await call('Network.setCacheDisabled', { cacheDisabled: true });
    ({ frameTree: { frame: { id: frameId } } } = await call('Page.getFrameTree'));
    const navigation = await call('Page.navigate', { url });
    if (navigation.errorText) failure = navigation.errorText;
    else await Promise.race([loaded, new Promise((_, reject) => {
      deadline = setTimeout(() => reject(Error('Navigation timeout')), 30000);
    })]);
    const evidence = echEvidence(url, responses);
    console.log(JSON.stringify({ ...evidence, navigationError: failure || undefined }, null, 2));
    if (evidence.result !== 'accepted' || failure) process.exitCode = 1;
  } finally {
    clearTimeout(deadline);
    for (const waiter of pending.values()) { clearTimeout(waiter.timer); waiter.reject(Error('CDP session closed')); }
    socket.close();
    // Leave the tab open for packet/log correlation; never close existing tabs.
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch(error => { console.error(error.message); process.exitCode = 1; });
}
