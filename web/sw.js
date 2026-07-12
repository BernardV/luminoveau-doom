// DOOM PWA service worker.
//
// Enables install ("Add to Home Screen") and offline play. The wasm/data are
// large + cache-busted with ?v=... query strings, so we use a runtime
// cache-first strategy (cache whatever gets fetched) rather than precaching a
// fixed list. Bump CACHE to invalidate everything on a new deploy.

// Cache name carries the build version (replaced at build time) so activating a
// new build drops every old entry.
const CACHE = 'doom-__DOOM_VERSION__';

self.addEventListener('install', () => {
    self.skipWaiting();
});

self.addEventListener('activate', (e) => {
    e.waitUntil(
        caches.keys().then((keys) =>
            Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)))
        ).then(() => self.clients.claim())
    );
});

self.addEventListener('fetch', (e) => {
    const req = e.request;
    if (req.method !== 'GET') return;

    // The HTML entry (navigation) is network-first: always fetch the latest page
    // so the shown build version is current; fall back to cache when offline.
    if (req.mode === 'navigate') {
        e.respondWith(
            fetch(req).then((res) => {
                caches.open(CACHE).then((c) => c.put(req, res.clone())).catch(() => {});
                return res;
            }).catch(() => caches.match(req, { ignoreSearch: true }))
        );
        return;
    }

    // Static assets (wasm/data/js/icons) are cache-first — they're versioned via
    // the ?v= query, so a new build fetches fresh URLs anyway. Keys ignore the
    // query so an unchanged asset still hits the cache across builds.
    e.respondWith(
        caches.open(CACHE).then((cache) =>
            cache.match(req, { ignoreSearch: true }).then((hit) => {
                if (hit) return hit;
                return fetch(req).then((res) => {
                    if (res && res.ok && res.type === 'basic') {
                        // Fire-and-forget; ignore quota errors (iOS caps SW cache
                        // and the wasm+data are large) so they never break loading.
                        cache.put(req, res.clone()).catch(() => {});
                    }
                    return res;
                }).catch(() => hit);
            })
        )
    );
});
