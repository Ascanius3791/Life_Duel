// Life Duel - the battle engine of the static build, in a Web Worker so long
// simulations never block the page. Loads the C++ engine compiled to
// WebAssembly (battle/wasm_engine.cpp) and runs the calls static-backend.js
// sends: { id, fn, args } -> { id, json } or { id, error }.
/* global createLifeDuelEngine */
'use strict';

importScripts('engine/life_duel_engine.js');

const ready = createLifeDuelEngine({ locateFile: file => `engine/${file}` }).then(m => {
  // Strings go through the heap: boards can be far larger than the wasm stack.
  const call = (fn, args) => {
    const ptrs = [];
    const raw = args.map(a => {
      if (typeof a !== 'string') return a;
      const len = m.lengthBytesUTF8(a) + 1;
      const p = m._malloc(len);
      m.stringToUTF8(a, p, len);
      ptrs.push(p);
      return p;
    });
    try {
      return m.UTF8ToString(fn(...raw));
    } finally {
      for (const p of ptrs) m._free(p);
    }
  };
  return {
    config: () => call(m._engine_config, []),
    battlefield: (preset, w, h, count, seed) => call(m._engine_battlefield, [preset, w, h, count, seed]),
    battle: (w, h, maxgen, variant, home, solo, board) => call(m._engine_battle, [w, h, maxgen, variant, home, solo, board]),
    step: (id, n, ms, board) => call(m._engine_step, [id, n, ms, board ? 1 : 0]),
  };
});

self.onmessage = async e => {
  const { id, fn, args } = e.data;
  try {
    const engine = await ready;
    if (!engine[fn]) throw new Error(`unknown engine call ${fn}`);
    self.postMessage({ id, json: engine[fn](...args) });
  } catch (err) {
    self.postMessage({ id, error: String((err && err.message) || err) });
  }
};
