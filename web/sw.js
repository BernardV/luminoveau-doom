// DOOM PWA service worker.
//
// Enables install ("Add to Home Screen") and offline play. The wasm/data are
// large + cache-busted with ?v=... query strings, so we use a runtime
// cache-first strategy (cache whatever gets fetched) rather than precaching a
// fixed list. Bump CACHE to invalidate everything on a new deploy.

const CACHE = 'doom-v1';

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

    // Cache-first, falling back to network and caching the result. Keys ignore
    // the ?v= cache-buster so re-visits with a new query still hit the cache.
    e.respondWith(
        caches.open(CACHE).then((cache) =>
            cache.match(req, { ignoreSearch: true }).then((hit) => {
                if (hit) return hit;
                return fetch(req).then((res) => {
                    if (res && res.ok && res.type === 'basic') {
                        cache.put(req, res.clone());
                    }
                    return res;
                }).catch(() => hit);
            })
        )
    );
});
