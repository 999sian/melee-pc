// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's threads need SharedArrayBuffer, which needs a cross-origin
// isolated page (COOP/COEP headers). Static hosts such as GitHub Pages cannot
// send headers, so this service worker adds them to every response in its
// scope. shell.mjs registers it only when the page is not isolated already;
// tools/browser/serve.py sends the headers itself.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));
self.addEventListener('fetch', (event) => {
  const { request } = event;
  if (request.cache === 'only-if-cached' && request.mode !== 'same-origin') return;
  event.respondWith(fetch(request).then((response) => {
    if (response.status === 0) return response; // opaque: not ours to rewrite
    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
  }));
});
