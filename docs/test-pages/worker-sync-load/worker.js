// importScripts and synchronous XHR move URLLoaderThrottles between sequences.
importScripts('imported.js');
if (typeof ServiceWorkerGlobalScope !== 'undefined' &&
    self instanceof ServiceWorkerGlobalScope) {
  self.addEventListener('install', event => event.waitUntil(self.skipWaiting()));
  self.addEventListener('message', event => {
    event.ports[0].postMessage({imported: self.importedMarker});
  });
} else {
  const request = new XMLHttpRequest();
  request.open('GET', 'imported.js?xhr', false);
  request.send();
  self.postMessage({imported: self.importedMarker, status: request.status,
                   bodyMatches: request.responseText.includes('worker-import-ok')});
}
