// Offline cache for the app (only active on https or localhost).
// Other apps may live on the same site, so this only ever touches caches named wbk-*.
const CACHE = 'wbk-v2';
const SHELL = ['./', 'index.html', 'style.css', 'app.js', 'zip.js', 'core/brick.js', 'core/brick.wasm', 'devices/index.json', 'manifest.webmanifest', 'icon.svg', 'icon-192.png'];
self.addEventListener('install', (e) => e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting())));
self.addEventListener('activate', (e) => e.waitUntil(caches.keys().then((ks) => Promise.all(ks.filter((k) => k.startsWith('wbk-') && k !== CACHE).map((k) => caches.delete(k)))).then(() => self.clients.claim())));
self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  if (url.origin !== location.origin || e.request.method !== 'GET') return;   // ROM downloads from GitHub are not cached here
  // network first (so updates show up), fall back to the cache when offline; faceplates are cached as they are played
  e.respondWith(fetch(e.request).then((r) => { if (r.ok) { const copy = r.clone(); caches.open(CACHE).then((c) => c.put(e.request, copy)); } return r; }).catch(() => caches.match(e.request)));
});
