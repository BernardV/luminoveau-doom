// DOOM PWA service worker.
//
// Network-first for everything: always try the network so a fresh deploy is picked
// up immediately, and fall back to the cache only when offline. (An earlier
// cache-first version with ignoreSearch served a stale doom.data across version
// bumps — the exact bug that kept a broken build alive.) It never resolves to
// undefined, which would make the browser throw "Failed to convert value to
// 'Response'". The cache is just an offline copy; the version-stamped cache name
// drops old entries on activate.

const CACHE = 'doom-__DOOM_VERSION__';

self.addEventListener('install', () => self.skipWaiting());

self.addEventListener('activate', (e) => {
    e.waitUntil(
        caches.keys()
            .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
            .then(() => self.clients.claim())
    );
});

self.addEventListener('fetch', (e) => {
    const req = e.request;
    if (req.method !== 'GET') return;

    e.respondWith(
        fetch(req)
            .then((res) => {
                // Cache a copy for offline use (best-effort; ignore quota errors —
                // iOS caps SW storage and the wasm+data are large).
                if (res && res.ok && res.type === 'basic') {
                    const copy = res.clone();
                    caches.open(CACHE).then((c) => c.put(req, copy)).catch(() => {});
                }
                return res;
            })
            .catch(() =>
                // Offline: serve the cached copy, or a proper error Response (never
                // undefined). Match with ignoreSearch so a ?v= asset still resolves.
                caches.match(req, { ignoreSearch: true }).then((hit) => hit || Response.error())
            )
    );
});
