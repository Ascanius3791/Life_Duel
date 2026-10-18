// Life Duel - backend of the static build (GitHub Pages), where there is no
// life_battle server. tools/build_site.sh includes this script before app.js;
// app.js then sends its /api/... requests here instead of over HTTP. The
// simulation runs in engine-worker.js (the C++ engine as WebAssembly), the
// bot leagues are static files listed in leagues/index.json.
'use strict';

(() => {
  const worker = new Worker('engine-worker.js');
  const pending = new Map();
  let nextId = 1;

  worker.onmessage = e => {
    const { id, json, error } = e.data;
    const p = pending.get(id);
    if (!p) return;
    pending.delete(id);
    if (error) return p.reject(new Error(error));
    const data = JSON.parse(json);
    if (data.error) p.reject(new Error(data.error));
    else p.resolve(data);
  };
  worker.onerror = e => {
    const err = new Error(`The battle engine failed to load: ${e.message || 'unknown error'}`);
    for (const p of pending.values()) p.reject(err);
    pending.clear();
  };

  const call = (fn, ...args) => new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    worker.postMessage({ id, fn, args });
  });

  let leagues = null;  // [{ id, file }]
  async function leagueIndex() {
    if (!leagues) {
      try {
        const res = await fetch('leagues/index.json');
        leagues = res.ok ? await res.json() : [];
      } catch { leagues = []; }
    }
    return leagues;
  }

  // The same requests and answers as the routes of battle/main.cpp.
  async function request(path, options = {}) {
    const url = new URL(path, location.href);
    const q = url.searchParams;
    const num = (key, fallback) => (q.has(key) && q.get(key) !== '' ? +q.get(key) : fallback);
    const route = url.pathname.slice(url.pathname.lastIndexOf('/api/') + 5);
    switch (route) {
      case 'config':
        return call('config');
      case 'battlefield':
        return call('battlefield', q.get('preset') || 'random_mirror', num('w', 260), num('h', 128), num('count', 800), num('seed', 1));
      case 'battle':
        return call('battle', num('w', 260), num('h', 128), num('maxgen', 2000), q.get('variant') || '', num('home', 1), num('solo', 0), String(options.body || ''));
      case 'step':
        return call('step', q.get('id') || '', num('n', 1), q.has('ms') ? Math.max(1, num('ms', 20)) : 0, q.get('board') !== '0');
      case 'bots':
        return { bots: (await leagueIndex()).map(b => ({ id: b.id })) };
      case 'bot': {
        const entry = (await leagueIndex()).find(b => b.id === q.get('id'));
        if (!entry) throw new Error('unknown bot');
        const res = await fetch(`leagues/${encodeURIComponent(entry.file)}`);
        if (!res.ok) throw new Error(`cannot load the league (${res.status})`);
        return res.json();
      }
      default:
        throw new Error(`unknown request ${url.pathname}`);
    }
  }

  window.lifeDuelBackend = { request, static: true };
})();
