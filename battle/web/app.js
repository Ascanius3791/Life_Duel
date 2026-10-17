'use strict';

// Life Duel - browser front end. The C++ server (life_battle) generates the
// battlefields and runs the simulation; this file handles placement,
// rendering and playback.

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

const DEAD = 0, RED = 1, BLUE = 2, GREY = 3;
const OWNER_CELL = { red: RED, blue: BLUE };
const COLOR = { red: '#ff4d6d', blue: '#3fa7ff', grey: '#8d96a8' };
const RGB = { [RED]: [255, 77, 109], [BLUE]: [63, 167, 255], [GREY]: [141, 150, 168] };
const SPEED_MIN = 1, SPEED_MAX = 1000;
const SNAP_DISTANCE = 12;  // cells beyond the own zone in which a dragged pattern still snaps into it

const S = {
  config: null,
  arsenal: new Map(),
  settings: { sizeId: 'medium', w: 260, h: 128, budget: 120, maxGen: 2000, unlimited: false, variant: 'birth', blind: true },
  field: { preset: 'random_mirror', count: 900, seed: newSeed() },
  grey: null,
  placements: [],
  nextId: 1,
  orient: { red: { rot: 0, mirror: false }, blue: { rot: 0, mirror: true } },
  phase: 'prep',        // 'prep' | 'battle'
  turn: 'red',          // blind placement: 'red' | 'blue' | 'done'
  ready: { red: false, blue: false },
  open: 'red',          // whose arsenal drawer is unfolded: 'red' | 'blue' | null
  focus: false,         // focus mode: the board fills the screen
  drag: null,           // pattern currently being placed
  pointer: null,        // last known pointer cell on the board
  battle: null,
  speed: 30,
  dirty: true,
  mode: 'duo',          // 'duo': two players at one screen | 'bot': one player against a trained bot | 'online': see online.js
  bots: [],             // leagues the server offers: [{ id }]
  bot: { id: null, league: null, side: 'blue', level: 'hard', army: null, limit: null },
  duo: null,            // the two-player settings, kept aside while vs Bot or online mode replaces them
};

function newSeed() { return Math.floor(Math.random() * 1e9); }

// ---------------------------------------------------------------------------
// Persistence of small preferences (per browser, optional)
// ---------------------------------------------------------------------------
function loadPrefs() {
  try {
    const p = JSON.parse(localStorage.getItem('lifeDuel.prefs') || '{}');
    if (p.settings) Object.assign(S.settings, p.settings);
    if (p.field) Object.assign(S.field, { preset: p.field.preset, count: p.field.count });
    if (p.speed) S.speed = p.speed;
    if (p.mode) S.mode = p.mode;
    if (p.bot) Object.assign(S.bot, { id: p.bot.id, side: p.bot.side === 'red' ? 'red' : 'blue', level: p.bot.level === 'easy' ? 'easy' : 'hard' });
    if (p.bot && p.bot.limit) S.bot.limit = { maxGen: +p.bot.limit.maxGen || 2000, unlimited: !!p.bot.limit.unlimited };
  } catch { /* storage unavailable */ }
}
function savePrefs() {
  // In vs Bot mode the league dictates the settings; the two-player ones are what is worth keeping.
  const { settings, field } = S.mode !== 'duo' && S.duo ? S.duo : S;
  try {
    localStorage.setItem('lifeDuel.prefs', JSON.stringify({
      settings, field: { preset: field.preset, count: field.count }, speed: S.speed,
      mode: S.mode === 'online' ? 'duo' : S.mode, bot: { id: S.bot.id, side: S.bot.side, level: S.bot.level, limit: S.bot.limit },
    }));
  } catch { /* storage unavailable */ }
}

// ---------------------------------------------------------------------------
// Server communication
// ---------------------------------------------------------------------------
async function api(path, options) {
  // The static build (GitHub Pages) has no server: static-backend.js answers in the browser.
  if (window.lifeDuelBackend) return window.lifeDuelBackend.request(path, options);
  const res = await fetch(path, options);
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || res.statusText);
  return data;
}

function decodeBoard(text, size) {
  const out = new Uint8Array(size);
  const value = { '.': DEAD, r: RED, b: BLUE, g: GREY };
  const re = /(\d*)([.rbg])/g;
  let pos = 0, m;
  while ((m = re.exec(text))) {
    const n = m[1] ? +m[1] : 1;
    out.fill(value[m[2]], pos, pos + n);
    pos += n;
  }
  return out;
}

function encodeBoard(board) {
  const sym = '.rbg';
  let out = '';
  for (let i = 0; i < board.length;) {
    let j = i + 1;
    while (j < board.length && board[j] === board[i]) j++;
    out += (j - i > 1 ? j - i : '') + sym[board[i]];
    i = j;
  }
  return out;
}

let toastTimer = 0;
function toast(message) {
  const el = $('#toast');
  el.textContent = message;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 3500);
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------
function zones(w = S.settings.w) {
  const zw = Math.floor(w / 5);
  return { zw, redEnd: zw, blueStart: w - zw };
}

function zoneRange(owner, w = S.settings.w) {
  const z = zones(w);
  return owner === 'red' ? [0, z.redEnd] : [z.blueStart, w];
}

const orientCache = new Map();
function oriented(pattern, o) {
  const key = `${pattern.id}|${o.rot}|${o.mirror}`;
  let shape = orientCache.get(key);
  if (shape) return shape;
  let cells = pattern.cells.map(([x, y]) => [x, y]);
  for (let i = 0; i < o.rot; i++) cells = cells.map(([x, y]) => [-y, x]);
  if (o.mirror) cells = cells.map(([x, y]) => [-x, y]);
  const minX = Math.min(...cells.map(c => c[0])), minY = Math.min(...cells.map(c => c[1]));
  cells = cells.map(([x, y]) => [x - minX, y - minY]);
  shape = {
    cells,
    w: Math.max(...cells.map(c => c[0])) + 1,
    h: Math.max(...cells.map(c => c[1])) + 1,
    rot: o.rot,
    mirror: !!o.mirror,
  };
  orientCache.set(key, shape);
  return shape;
}

// ---------------------------------------------------------------------------
// Placement bookkeeping
// ---------------------------------------------------------------------------
const spent = owner => S.placements.reduce((sum, p) => sum + (p.owner === owner ? p.cost : 0), 0);
const remaining = owner => S.settings.budget - spent(owner);

function isActive(owner) {
  if (S.phase !== 'prep' || isBot(owner) || isRemote(owner)) return false;
  return S.settings.blind ? S.turn === owner : !S.ready[owner];
}

// The owner that keyboard shortcuts apply to when nothing is being dragged.
function focusOwner() {
  if (S.drag) return S.drag.owner;
  if (S.open && isActive(S.open)) return S.open;
  if (S.settings.blind) return S.turn === 'done' ? null : S.turn;
  const active = ['red', 'blue'].filter(isActive);
  return active.length === 1 ? active[0] : null;
}

function isHidden(owner) {
  if (S.mode === 'bot') return S.phase === 'prep' && isBot(owner);
  if (S.mode === 'online') return S.phase === 'prep' && isRemote(owner);
  return S.phase === 'prep' && S.settings.blind && S.turn !== 'done' && S.turn !== owner;
}

// Where a pattern would land with the pointer over `cell`.
function placementFor(owner, shape, cell, cost) {
  const { w, h } = S.settings;
  const [zx0, zx1] = zoneRange(owner);
  let x = cell.x - Math.floor(shape.w / 2);
  let y = cell.y - Math.floor(shape.h / 2);
  y = Math.max(0, Math.min(h - shape.h, y));
  // Snap into the own zone when the pointer is in it, close to it, or beyond the board edge behind it.
  const nearZone = owner === 'red' ? cell.x < zx1 + SNAP_DISTANCE : cell.x >= zx0 - SNAP_DISTANCE;
  if (nearZone) x = Math.max(zx0, Math.min(zx1 - shape.w, x));
  const fits = x >= zx0 && x + shape.w <= zx1 && y >= 0 && y + shape.h <= h && shape.w <= zx1 - zx0;
  const affordable = cost <= remaining(owner);
  return { x, y, valid: fits && affordable && x >= 0 && x + shape.w <= w, affordable };
}

function place(owner, pattern, shape, x, y) {
  // Avoid paying twice for cells that are already alive in the own colour.
  const board = prepBoard();
  const own = OWNER_CELL[owner];
  const { w } = S.settings;
  if (shape.cells.every(([cx, cy]) => board[(y + cy) * w + x + cx] === own)) return false;
  S.placements.push({ id: S.nextId++, owner, patternId: pattern.id, cost: pattern.cost, x, y, w: shape.w, h: shape.h, cells: shape.cells, rot: shape.rot, mirror: shape.mirror });
  boardChanged();
  return true;
}

function placementAt(owner, cell) {
  for (let i = S.placements.length - 1; i >= 0; i--) {
    const p = S.placements[i];
    if (p.owner !== owner) continue;
    if (cell.x >= p.x && cell.x < p.x + p.w && cell.y >= p.y && cell.y < p.y + p.h) return p;
  }
  return null;
}

function removePlacement(p) {
  S.placements = S.placements.filter(q => q !== p);
  boardChanged();
}

// The board as placed so far, plus `extra` placements (the bot's army) that are not part of S.placements.
function prepBoard(extra = []) {
  const { w } = S.settings;
  const board = S.grey ? S.grey.slice() : new Uint8Array(S.settings.w * S.settings.h);
  for (const p of S.placements.concat(extra)) {
    const v = OWNER_CELL[p.owner];
    for (const [cx, cy] of p.cells) board[(p.y + cy) * w + p.x + cx] = v;
  }
  return board;
}

function boardChanged() {
  if (S.phase === 'prep') paintBoard(prepBoard());
  updateSides();
  updateScoreboard();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
const canvas = $('#board');
const ctx = canvas.getContext('2d');
const off = document.createElement('canvas');
const offCtx = off.getContext('2d');
let offImage = null, offPixels = null;
let view = { cs: 1, ox: 0, oy: 0, dpr: 1 };
let gridLayer = null;

const pack = ([r, g, b], a = 255) => ((a << 24) | (b << 16) | (g << 8) | r) >>> 0;
const PACKED = new Uint32Array([0, pack(RGB[RED]), pack(RGB[BLUE]), pack(RGB[GREY])]);

function paintBoard(board) {
  const { w, h } = S.settings;
  if (off.width !== w || off.height !== h || !offImage) {
    off.width = w;
    off.height = h;
    offImage = offCtx.createImageData(w, h);
    offPixels = new Uint32Array(offImage.data.buffer);
  }
  for (let i = 0; i < board.length; i++) offPixels[i] = PACKED[board[i]];
  offCtx.putImageData(offImage, 0, 0);
  S.dirty = true;
}

function resize() {
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.max(1, Math.round(rect.width * dpr));
  canvas.height = Math.max(1, Math.round(rect.height * dpr));
  const { w, h } = S.settings;
  let cs = Math.min(canvas.width / w, canvas.height / h);
  if (cs >= 3) cs = Math.floor(cs);
  view = { cs, ox: Math.floor((canvas.width - w * cs) / 2), oy: Math.floor((canvas.height - h * cs) / 2), dpr };

  gridLayer = null;
  if (cs >= 4) {
    gridLayer = document.createElement('canvas');
    gridLayer.width = canvas.width;
    gridLayer.height = canvas.height;
    const g = gridLayer.getContext('2d');
    g.fillStyle = 'rgba(10, 13, 19, 0.6)';
    const lw = Math.max(1, Math.round(cs * 0.12));
    for (let x = 0; x <= w; x++) g.fillRect(view.ox + Math.round(x * cs) - (lw >> 1), view.oy, lw, h * cs);
    for (let y = 0; y <= h; y++) g.fillRect(view.ox, view.oy + Math.round(y * cs) - (lw >> 1), w * cs, lw);
  }
  S.dirty = true;
  layoutDrawers();
}

function render() {
  S.dirty = false;
  const { w, h } = S.settings;
  const { cs, ox, oy, dpr } = view;
  const W = w * cs, H = h * cs;
  const z = zones();

  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = '#0c1018';
  ctx.fillRect(ox, oy, W, H);

  // Player zones
  ctx.fillStyle = 'rgba(255, 77, 109, 0.075)';
  ctx.fillRect(ox, oy, z.redEnd * cs, H);
  ctx.fillStyle = 'rgba(63, 167, 255, 0.075)';
  ctx.fillRect(ox + z.blueStart * cs, oy, z.zw * cs, H);

  // Cells, gaps and glow
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(off, ox, oy, W, H);
  if (gridLayer) ctx.drawImage(gridLayer, 0, 0);
  ctx.save();
  ctx.globalCompositeOperation = 'lighter';
  ctx.globalAlpha = 0.4;
  ctx.filter = `blur(${Math.max(2, cs * 0.9)}px)`;
  ctx.imageSmoothingEnabled = true;
  ctx.drawImage(off, ox, oy, W, H);
  ctx.restore();

  // Zone borders
  ctx.save();
  ctx.setLineDash([6 * dpr, 6 * dpr]);
  ctx.lineWidth = dpr;
  ctx.strokeStyle = 'rgba(255, 77, 109, 0.45)';
  line(ox + z.redEnd * cs, oy, ox + z.redEnd * cs, oy + H);
  ctx.strokeStyle = 'rgba(63, 167, 255, 0.45)';
  line(ox + z.blueStart * cs, oy, ox + z.blueStart * cs, oy + H);
  ctx.restore();
  ctx.strokeStyle = '#2a3349';
  ctx.lineWidth = dpr;
  ctx.strokeRect(ox - 0.5, oy - 0.5, W + 1, H + 1);

  // Fog over the hidden army in blind mode
  for (const owner of ['red', 'blue']) {
    if (!isHidden(owner)) continue;
    const [x0, x1] = zoneRange(owner);
    drawFog(ox + x0 * cs, oy, (x1 - x0) * cs, H, owner);
  }

  if (S.phase === 'prep') renderPrepOverlay();
}

function line(x0, y0, x1, y1) {
  ctx.beginPath();
  ctx.moveTo(x0, y0);
  ctx.lineTo(x1, y1);
  ctx.stroke();
}

function drawFog(x, y, w, h, owner) {
  const { dpr } = view;
  ctx.save();
  ctx.beginPath();
  ctx.rect(x, y, w, h);
  ctx.clip();
  ctx.fillStyle = '#10141f';
  ctx.fillRect(x, y, w, h);
  ctx.strokeStyle = owner === 'red' ? 'rgba(255, 77, 109, 0.12)' : 'rgba(63, 167, 255, 0.12)';
  ctx.lineWidth = 6 * dpr;
  for (let k = -h; k < w + h; k += 22 * dpr) line(x + k, y, x + k + h, y + h);
  ctx.fillStyle = owner === 'red' ? COLOR.red : COLOR.blue;
  ctx.font = `700 ${14 * dpr}px ${getComputedStyle(document.body).fontFamily}`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(isBot(owner) ? "THE BOT'S ARMY IS HIDDEN" : `${owner.toUpperCase()} IS HIDDEN`, x + w / 2, y + h / 2);
  ctx.restore();
}

function renderPrepOverlay() {
  const { cs, ox, oy, dpr } = view;
  const d = S.drag;
  if (d && S.pointer && S.pointer.onCanvas) {
    const shape = oriented(d.pattern, S.orient[d.owner]);
    const target = placementFor(d.owner, shape, S.pointer, d.pattern.cost);
    ctx.save();
    ctx.globalAlpha = target.valid ? 0.75 : 0.45;
    ctx.fillStyle = target.valid ? COLOR[d.owner] : '#ffffff';
    const inset = cs >= 4 ? Math.max(1, cs * 0.12) : 0;
    for (const [cx, cy] of shape.cells) {
      ctx.fillRect(ox + (target.x + cx) * cs + inset / 2, oy + (target.y + cy) * cs + inset / 2, cs - inset, cs - inset);
    }
    ctx.globalAlpha = 1;
    ctx.lineWidth = 1.5 * dpr;
    ctx.setLineDash(target.valid ? [] : [4 * dpr, 4 * dpr]);
    ctx.strokeStyle = target.valid ? COLOR[d.owner] : '#ff6b6b';
    ctx.strokeRect(ox + target.x * cs - 2 * dpr, oy + target.y * cs - 2 * dpr, shape.w * cs + 4 * dpr, shape.h * cs + 4 * dpr);
    ctx.restore();
    return;
  }
  const hover = hoveredPlacement();
  if (hover) {
    ctx.save();
    ctx.setLineDash([4 * dpr, 3 * dpr]);
    ctx.lineWidth = 1.5 * dpr;
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.8)';
    ctx.strokeRect(ox + hover.x * cs - 2 * dpr, oy + hover.y * cs - 2 * dpr, hover.w * cs + 4 * dpr, hover.h * cs + 4 * dpr);
    ctx.restore();
  }
}

function hoveredPlacement() {
  if (S.phase !== 'prep' || S.drag || !S.pointer || !S.pointer.inside) return null;
  for (const owner of ['red', 'blue']) {
    if (!isActive(owner)) continue;
    const p = placementAt(owner, S.pointer);
    if (p) return p;
  }
  return null;
}

function updateHint() {
  const el = $('#boardHint');
  let text = '';
  if (S.phase === 'prep') {
    const hover = hoveredPlacement();
    if (S.drag) text = 'R / Shift+R rotate · F mirror · V flip · wheel rotates · Esc or right-click to stop';
    else if (hover) text = `Right-click to remove ${S.arsenal.get(hover.patternId).name} (+${hover.cost} pts)`;
  }
  el.textContent = text;
  el.classList.toggle('show', !!text);
  canvas.style.cursor = S.drag ? 'crosshair' : (hoveredPlacement() ? 'pointer' : 'default');
}

// ---------------------------------------------------------------------------
// Arsenal panels
// ---------------------------------------------------------------------------
function buildCards() {
  for (const owner of ['red', 'blue']) {
    const container = $(`#side${cap(owner)} .cards`);
    container.innerHTML = '';
    for (const pattern of S.config.arsenal) {
      const card = document.createElement('div');
      card.className = 'card';
      card.dataset.id = pattern.id;
      card.innerHTML = `<span class="cost">${pattern.cost}</span><canvas></canvas><span class="name">${pattern.name}</span>`;
      card.addEventListener('pointerdown', e => onCardDown(e, owner, pattern, card));
      card.addEventListener('pointerenter', e => showTooltip(e, pattern));
      card.addEventListener('pointermove', moveTooltip);
      card.addEventListener('pointerleave', hideTooltip);
      container.appendChild(card);
    }
    drawCardPreviews(owner);
  }
}

function drawCardPreviews(owner) {
  const dpr = window.devicePixelRatio || 1;
  for (const card of $$(`#side${cap(owner)} .card`)) {
    const pattern = S.arsenal.get(card.dataset.id);
    const shape = oriented(pattern, S.orient[owner]);
    const c = $('canvas', card);
    const cw = 64, ch = 36;
    c.width = cw * dpr;
    c.height = ch * dpr;
    const g = c.getContext('2d');
    g.scale(dpr, dpr);
    const s = Math.min((cw - 4) / shape.w, (ch - 4) / shape.h, 7);
    const x0 = (cw - shape.w * s) / 2, y0 = (ch - shape.h * s) / 2;
    g.fillStyle = COLOR[owner];
    g.shadowColor = COLOR[owner];
    g.shadowBlur = 4;
    const gap = s >= 3 ? 0.6 : 0;
    for (const [x, y] of shape.cells) g.fillRect(x0 + x * s, y0 + y * s, s - gap, s - gap);
  }
}

function updateSides() {
  for (const owner of ['red', 'blue']) {
    const side = $(`#side${cap(owner)}`);
    const left = remaining(owner);
    const { budget } = S.settings;
    const hidden = isHidden(owner);
    $('.budget-fill', side).style.width = `${Math.max(0, left / budget) * 100}%`;
    $('.budget-text', side).innerHTML = hidden ? '<b>?</b> pts' : `<b>${left}</b> / ${budget} pts`;
    const active = isActive(owner);
    const open = S.open === owner && active;
    side.classList.toggle('folded', !open);
    side.classList.toggle('is-ready', S.ready[owner]);
    side.inert = !open;
    const tab = $(`#tab${cap(owner)}`);
    tab.classList.toggle('open', open);
    tab.disabled = !active;
    // The bot has no arsenal to open, and its tab would cover the human's zone.
    tab.hidden = isBot(owner) || isRemote(owner);
    $('.tab-label', tab).textContent = S.mode === 'bot' || S.mode === 'online' ? `${cap(owner)} · you` : cap(owner);
    $('.tab-state', tab).textContent = S.ready[owner] ? 'ready ✓' : (active ? 'arsenal' : 'waiting');
    tab.title = active ? `Open ${cap(owner)}'s arsenal (A)` : (S.ready[owner] ? `${cap(owner)} is ready` : `${cap(owner)} places after ${owner === 'red' ? 'Blue' : 'Red'}`);
    for (const card of $$('.card', side)) {
      const pattern = S.arsenal.get(card.dataset.id);
      card.classList.toggle('unaffordable', pattern.cost > left);
      card.classList.toggle('selected', !!S.drag && S.drag.owner === owner && S.drag.pattern === pattern);
    }
    const readyBtn = $('[data-act=ready]', side);
    const hasArmy = S.placements.some(p => p.owner === owner);
    if (S.settings.blind) {
      readyBtn.textContent = S.ready[owner] ? 'Done ✓' : (S.turn === owner ? 'Ready — hide' : 'Waiting');
      readyBtn.disabled = S.turn !== owner;
    } else {
      readyBtn.textContent = S.ready[owner] ? 'Ready ✓' : 'Ready';
      readyBtn.disabled = S.phase !== 'prep' || (S.mode === 'online' && S.ready[owner]);
    }
    readyBtn.title = hasArmy ? '' : 'You have not placed anything yet - you would lose immediately!';
    $('[data-act=undo]', side).disabled = !active || !hasArmy;
    $('[data-act=clear]', side).disabled = !active || !hasArmy;
  }
  $('#btnStart').disabled = !(S.ready.red && S.ready.blue);
  document.body.classList.toggle('dragging', !!S.drag);
  updateFocusAvailability();
  updatePhaseLabel();
}

function updatePhaseLabel() {
  const el = $('#phaseLabel');
  const R = '<b class="red">Red</b>', B = '<b class="blue">Blue</b>';
  if (S.phase === 'battle') {
    const b = S.battle;
    if (!b) return;
    const f = b.frame;
    if (f.finished) {
      const who = S.mode === 'bot' && f.winner !== 'draw' ? (isBot(f.winner) ? ' (the bot)' : ' (you)')
        : S.mode === 'online' && f.winner !== 'draw' ? (isRemote(f.winner) ? ' (your friend)' : ' (you)') : '';
      el.innerHTML = f.winner === 'draw' ? 'Battle over — draw' : `Battle over — ${f.winner === 'red' ? R : B}${who} wins`;
    } else {
      el.innerHTML = b.turbo ? 'Battle — max speed' : (b.playing ? 'Battle in progress' : 'Battle paused');
    }
    return;
  }
  if (S.mode === 'online') {
    el.innerHTML = onlinePhaseLabel();
    return;
  }
  if (S.mode === 'bot') {
    const you = humanSide() === 'red' ? R : B;
    el.innerHTML = S.ready[humanSide()] ? 'Your army is ready — start the battle!'
      : `Place your army, ${you} — the bot has already chosen its own in secret`;
    return;
  }
  if (S.settings.blind) {
    if (S.turn === 'red') el.innerHTML = `${R} places their army — ${B}, look away!`;
    else if (S.turn === 'blue') el.innerHTML = `${B} places their army — ${R}, look away!`;
    else el.innerHTML = 'Both armies are ready — start the battle!';
  } else {
    el.innerHTML = S.ready.red && S.ready.blue ? 'Both armies are ready — start the battle!' : `Place your armies, ${R} and ${B}`;
  }
}

function cap(s) { return s[0].toUpperCase() + s.slice(1); }

function changeOrientation(owner, act) {
  if (!owner) return;
  const o = { ...S.orient[owner] };
  if (act === 'rotr') o.rot = (o.rot + (o.mirror ? 3 : 1)) % 4;
  if (act === 'rotl') o.rot = (o.rot + (o.mirror ? 1 : 3)) % 4;
  if (act === 'mirror') o.mirror = !o.mirror;
  if (act === 'vflip') { o.mirror = !o.mirror; o.rot = (o.rot + 2) % 4; }
  S.orient[owner] = o;
  drawCardPreviews(owner);
  S.dirty = true;
}

// Only one arsenal is unfolded at a time; it opens over the opponent's area.
function openDrawer(owner) {
  if (owner && !isActive(owner)) return;
  if (S.drag && S.drag.owner !== owner) endDrag();
  S.open = owner;
  hideTooltip();
  updateSides();
  S.dirty = true;
}

function toggleDrawer() {
  if (S.phase !== 'prep') return;
  if (S.open && isActive(S.open)) return openDrawer(null);
  const candidates = S.settings.blind ? [S.turn] : ['red', 'blue'];
  openDrawer(candidates.find(isActive) || null);
}

// Places each arsenal over the zone of the opponent: Red's covers Blue's area
// at the right edge of the board, Blue's covers Red's area on the left.
function layoutDrawers() {
  const stage = $('#stage').getBoundingClientRect();
  const cr = canvas.getBoundingClientRect();
  if (!stage.width || !view.dpr) return;
  const k = 1 / view.dpr;
  const bx = cr.left - stage.left + view.ox * k, by = cr.top - stage.top + view.oy * k;
  const bw = S.settings.w * view.cs * k, bh = S.settings.h * view.cs * k;
  const zonePx = zones().zw * view.cs * k;
  const width = Math.min(Math.max(zonePx, 310), stage.width - 24);
  const height = Math.min(Math.max(bh, 420), stage.height - 12);
  const top = Math.max(6, Math.min(by, stage.height - height - 6));
  const right = Math.min(bx + bw, stage.width - 6), left = Math.max(bx, 6);
  const px = v => `${Math.round(v)}px`;
  Object.assign($('#sideRed').style, { left: px(right - width), top: px(top), width: px(width), height: px(height) });
  Object.assign($('#sideBlue').style, { left: px(left), top: px(top), width: px(width), height: px(height) });
  const tabTop = px(top + Math.min(24, height / 4));
  Object.assign($('#tabRed').style, { left: px(right - 34), top: tabTop });
  Object.assign($('#tabBlue').style, { left: px(left), top: tabTop });
}

function onSideAction(owner, act) {
  if (act === 'fold') return openDrawer(null);
  if (['rotl', 'rotr', 'mirror', 'vflip'].includes(act)) return changeOrientation(owner, act);
  if (act === 'undo') {
    const own = S.placements.filter(p => p.owner === owner);
    if (own.length) removePlacement(own[own.length - 1]);
  }
  if (act === 'clear') {
    S.placements = S.placements.filter(p => p.owner !== owner);
    boardChanged();
  }
  if (act === 'ready') setReady(owner);
}

function setReady(owner) {
  if (S.mode === 'online') return onlineSetReady(owner);
  if (S.phase !== 'prep') return;
  if (S.drag && S.drag.owner === owner) endDrag();
  const hasArmy = S.placements.some(p => p.owner === owner);
  if (!S.ready[owner] && !hasArmy && !confirm(`${cap(owner)} has not placed anything and would lose immediately. Ready anyway?`)) return;
  const other = owner === 'red' ? 'blue' : 'red';
  if (S.settings.blind) {
    if (S.turn !== owner) return;
    S.ready[owner] = true;
    S.turn = owner === 'red' ? 'blue' : 'done';
    S.open = S.turn === 'done' ? null : S.turn;
  } else {
    S.ready[owner] = !S.ready[owner];
    // Hand the screen over to the other player, or fold everything away once both are done.
    if (S.ready[owner]) S.open = S.ready[other] ? null : other;
    else S.open = owner;
  }
  updateSides();
  updateScoreboard();
  S.dirty = true;
}

function resetPreparation({ clearArmies }) {
  if (clearArmies) S.placements = [];
  S.phase = 'prep';
  S.turn = 'red';
  S.ready = { red: false, blue: false };
  S.drag = null;
  S.open = 'red';
  if (S.mode === 'bot') {
    S.ready[S.bot.side] = true;
    S.open = humanSide();
    S.bot.army = pickBotArmy();
  }
  if (S.mode === 'online') onlineResetPreparation();
  document.body.classList.remove('battle');
  $('#centerPrep').hidden = false;
  $('#centerBattle').hidden = true;
  boardChanged();
}

// ---------------------------------------------------------------------------
// Dragging patterns onto the board
// ---------------------------------------------------------------------------
function onCardDown(e, owner, pattern, card) {
  if (e.button !== 0 || !isActive(owner)) return;
  e.preventDefault();
  hideTooltip();
  if (pattern.cost > remaining(owner)) {
    toast(`Not enough points for ${pattern.name} (${pattern.cost} pts).`);
    return;
  }
  if (S.drag && S.drag.sticky && S.drag.pattern === pattern && S.drag.owner === owner) {
    endDrag();
    return;
  }
  S.drag = { owner, pattern, card, sticky: false, moved: false, startX: e.clientX, startY: e.clientY };
  updateSides();
  updateHint();
}

function endDrag() {
  S.drag = null;
  updateSides();
  updateHint();
  S.dirty = true;
}

function tryPlaceAtPointer() {
  const d = S.drag;
  if (!d || !S.pointer || !S.pointer.onCanvas) return false;
  const shape = oriented(d.pattern, S.orient[d.owner]);
  const target = placementFor(d.owner, shape, S.pointer, d.pattern.cost);
  if (!target.valid) {
    if (!target.affordable) toast('Not enough points left.');
    return false;
  }
  return place(d.owner, d.pattern, shape, target.x, target.y);
}

function pointerCell(e) {
  const rect = canvas.getBoundingClientRect();
  const onCanvas = e.clientX >= rect.left && e.clientX < rect.right && e.clientY >= rect.top && e.clientY < rect.bottom;
  const px = (e.clientX - rect.left) * view.dpr, py = (e.clientY - rect.top) * view.dpr;
  const x = Math.floor((px - view.ox) / view.cs), y = Math.floor((py - view.oy) / view.cs);
  const inside = x >= 0 && y >= 0 && x < S.settings.w && y < S.settings.h;
  return { x, y, inside, onCanvas };
}

document.addEventListener('pointermove', e => {
  const cell = pointerCell(e);
  const changed = !S.pointer || cell.x !== S.pointer.x || cell.y !== S.pointer.y || cell.onCanvas !== S.pointer.onCanvas;
  S.pointer = cell;
  const d = S.drag;
  if (d && !d.moved && Math.hypot(e.clientX - d.startX, e.clientY - d.startY) > 6) d.moved = true;
  // Painting single cells while holding the button in click-to-place mode.
  if (d && d.sticky && d.pattern.id === 'cell' && (e.buttons & 1) && changed) tryPlaceAtPointer();
  if (changed) {
    S.dirty = true;
    updateHint();
  }
});

document.addEventListener('pointerup', e => {
  const d = S.drag;
  if (!d || d.sticky || e.button !== 0) return;
  if (!d.moved) {
    d.sticky = true;  // a click on a card: keep placing until Esc / right-click
    updateHint();
    return;
  }
  tryPlaceAtPointer();
  endDrag();
});

canvas.addEventListener('pointerdown', e => {
  if (S.phase !== 'prep') return;
  S.pointer = pointerCell(e);
  if (e.button === 2) {
    if (S.drag) return endDrag();
    const p = hoveredPlacement();
    if (p) removePlacement(p);
    updateHint();
    return;
  }
  if (e.button === 0 && S.drag && S.drag.sticky) {
    tryPlaceAtPointer();
    if (S.drag.pattern.cost > remaining(S.drag.owner)) endDrag();
  }
});
canvas.addEventListener('contextmenu', e => e.preventDefault());
canvas.addEventListener('pointerleave', () => { S.pointer = null; S.dirty = true; updateHint(); });
canvas.addEventListener('wheel', e => {
  if (!S.drag) return;
  e.preventDefault();
  changeOrientation(S.drag.owner, e.deltaY > 0 ? 'rotr' : 'rotl');
}, { passive: false });

// ---------------------------------------------------------------------------
// Tooltip
// ---------------------------------------------------------------------------
function showTooltip(e, pattern) {
  if (S.drag) return;
  const el = $('#tooltip');
  el.innerHTML = `<span class="cat">${pattern.category} · ${pattern.width}×${pattern.height} · ${pattern.cells.length} cells</span>
    <b>${pattern.name} — ${pattern.cost} pts</b>${pattern.description}`;
  el.hidden = false;
  moveTooltip(e);
}
function moveTooltip(e) {
  const el = $('#tooltip');
  if (el.hidden) return;
  const r = el.getBoundingClientRect();
  el.style.left = `${Math.min(window.innerWidth - r.width - 8, e.clientX + 14)}px`;
  el.style.top = `${Math.max(8, e.clientY - r.height - 12)}px`;
}
function hideTooltip() { $('#tooltip').hidden = true; }

// ---------------------------------------------------------------------------
// Battlefield controls
// ---------------------------------------------------------------------------
let fieldRequest = 0;
async function loadBattlefield() {
  const { w, h } = S.settings;
  const { preset, count, seed } = S.field;
  const ticket = ++fieldRequest;
  try {
    const data = await api(`/api/battlefield?w=${w}&h=${h}&preset=${encodeURIComponent(preset)}&count=${count}&seed=${seed}`);
    if (ticket !== fieldRequest) return;
    S.grey = decodeBoard(data.board, w * h);
    boardChanged();
    onlineFieldLoaded();
  } catch (err) {
    toast(`Could not load the battlefield: ${err.message}`);
  }
}

function syncFieldControls() {
  const preset = S.config.presets.find(p => p.id === S.field.preset) || S.config.presets[0];
  S.field.preset = preset.id;
  $('#presetSelect').value = preset.id;
  $('#presetDesc').textContent = preset.description;
  $('#countRow').style.display = preset.usesCount ? '' : 'none';
  $('#countRange').value = Math.min(4000, S.field.count);
  $('#countInput').value = S.field.count;
  $('#btnReroll').disabled = !['random', 'random_mirror', 'random_rotational', 'reactor'].includes(preset.id);
}

let countTimer = 0;
function setCount(value) {
  S.field.count = Math.max(0, Math.min(20000, Math.round(+value || 0)));
  syncFieldControls();
  clearTimeout(countTimer);
  countTimer = setTimeout(() => { loadBattlefield(); savePrefs(); }, 120);
}

// ---------------------------------------------------------------------------
// Scoreboard and side stats
// ---------------------------------------------------------------------------
function fmt(n) { return Math.round(n).toLocaleString('en-US'); }

function updateScoreboard() {
  let red, blue, grey, redSub, blueSub;
  if (S.phase === 'battle' && S.battle) {
    const f = S.battle.frame;
    ({ red, blue, grey } = f);
    redSub = blueSub = '';
    const m = S.battle.mark;
    $('#genLabel').textContent = `Generation ${fmt(f.gen)} / ${f.maxGen ? fmt(f.maxGen) : '∞'}` +
      (m ? ` (bot trained to ${fmt(m.gen)})` : '');
    $('#genProgress').style.width = f.maxGen ? `${Math.min(100, (f.gen / f.maxGen) * 100)}%` : '0%';
    $('#genMark').hidden = !m || !f.maxGen;
    if (m && f.maxGen) $('#genMark').style.left = `${(m.gen / f.maxGen) * 100}%`;
    $('#genProgressBar').classList.toggle('endless', !f.maxGen && !f.finished);
  } else {
    const board = prepBoard();
    red = blue = grey = 0;
    for (const v of board) { red += v === RED; blue += v === BLUE; grey += v === GREY; }
    redSub = isHidden('red') ? '' : `${remaining('red')} pts left`;
    blueSub = isHidden('blue') ? '' : `${remaining('blue')} pts left`;
    $('#genLabel').textContent = 'Preparation';
    $('#genProgress').style.width = '0%';
    $('#genProgressBar').classList.remove('endless');
    $('#genMark').hidden = true;
    $('#rateLabel').textContent = S.settings.unlimited ? 'no generation limit' : `limit ${fmt(S.settings.maxGen)} gens`;
  }
  const showRed = !isHidden('red'), showBlue = !isHidden('blue');
  $('#scoreRed').textContent = showRed ? fmt(red) : '?';
  $('#scoreBlue').textContent = showBlue ? fmt(blue) : '?';
  $('#scoreRedSub').textContent = redSub;
  $('#scoreBlueSub').textContent = blueSub;
  $('#greyLabel').textContent = `${fmt(grey)} grey`;
  const total = (showRed ? red : 1) + (showBlue ? blue : 1);
  const share = !showRed || !showBlue || total === 0 ? 0.5 : red / total;
  $('#tugRed').style.width = `${share * 100}%`;
}

function drawSpark(owner) {
  const c = $(`#spark${cap(owner)}`);
  const data = S.battle ? S.battle.live[owner] : [];
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth, h = c.clientHeight;
  if (!w || !h) return;
  if (c.width !== Math.round(w * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(h * dpr); }
  const g = c.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  if (data.length < 2) return;
  const all = S.battle.live.red.concat(S.battle.live.blue);
  const max = Math.max(1, ...sample(all, 2000).map(p => p[1]));
  const lastGen = data[data.length - 1][0] || 1;
  const pts = sample(data, Math.round(w));
  const X = gen => (gen / lastGen) * (w - 2) + 1, Y = v => h - 2 - (v / max) * (h - 5);
  g.beginPath();
  pts.forEach(([gen, v], i) => (i ? g.lineTo(X(gen), Y(v)) : g.moveTo(X(gen), Y(v))));
  g.strokeStyle = COLOR[owner];
  g.lineWidth = 1.5;
  g.shadowColor = COLOR[owner];
  g.shadowBlur = 5;
  g.stroke();
  g.shadowBlur = 0;
  g.lineTo(X(pts[pts.length - 1][0]), h);
  g.lineTo(X(pts[0][0]), h);
  const grad = g.createLinearGradient(0, 0, 0, h);
  grad.addColorStop(0, owner === 'red' ? 'rgba(255,77,109,0.35)' : 'rgba(63,167,255,0.35)');
  grad.addColorStop(1, 'rgba(0,0,0,0)');
  g.fillStyle = grad;
  g.fill();
}

function sample(arr, n) {
  if (arr.length <= n) return arr;
  const step = arr.length / n, out = [];
  for (let i = 0; i < n; i++) out.push(arr[Math.floor(i * step)]);
  out.push(arr[arr.length - 1]);
  return out;
}

// ---------------------------------------------------------------------------
// Battle
// ---------------------------------------------------------------------------
async function startBattle(encoded) {
  const { w, h, maxGen, unlimited, variant } = S.settings;
  const botArmy = S.mode === 'bot' ? S.bot.army : null;
  if (S.mode === 'bot' && !botArmy) return toast('The bot has no army to play.');
  const extra = botArmy ? botPlacements(botArmy[S.bot.side], S.bot.side) : S.mode === 'online' ? onlineOpponentPlacements() : [];
  const body = encoded || encodeBoard(prepBoard(extra));
  try {
    const frame = await api(`/api/battle?w=${w}&h=${h}&maxgen=${unlimited ? 0 : maxGen}&variant=${variant}`, { method: 'POST', body });
    S.drag = null;
    S.phase = 'battle';
    S.turn = 'done';
    document.body.classList.add('battle');
    $('#centerPrep').hidden = true;
    $('#centerBattle').hidden = false;
    S.battle = {
      id: frame.id, frame, encoded: body, playing: true, turbo: false, acc: 0,
      pending: null, skipping: false, resultShown: false,
      live: { red: [], blue: [] }, rate: [], botArmy, mark: trainedMark(),
    };
    updateBotInfo();
    if (S.mode === 'online' && !encoded) onlineBattleStarted(body);
    applyFrame(frame);
    updateBattleControls();
    updateSides();
  } catch (err) {
    toast(`Could not start the battle: ${err.message}`);
  }
}

function applyFrame(frame) {
  const b = S.battle;
  b.frame = frame;
  if (b.mark && !b.mark.reached && frame.gen === b.mark.gen) {
    Object.assign(b.mark, { reached: true, red: frame.red, blue: frame.blue });
    if (!frame.finished && !b.skipping) toast(`Generation ${fmt(b.mark.gen)}: the bot was trained up to here — ${markStanding(b.mark)}. Playing on.`);
  }
  if (frame.board) paintBoard(decodeBoard(frame.board, S.settings.w * S.settings.h));
  const last = b.live.red[b.live.red.length - 1];
  if (frame.history) {
    // The final frame carries the (sampled) history, which also covers skipped generations.
    b.live.red = frame.history.red.map((v, i) => [frame.history.gen[i], v]);
    b.live.blue = frame.history.blue.map((v, i) => [frame.history.gen[i], v]);
  } else if (!last || last[0] !== frame.gen) {
    b.live.red.push([frame.gen, frame.red]);
    b.live.blue.push([frame.gen, frame.blue]);
  }
  const now = performance.now();
  b.rate.push([now, frame.gen]);
  while (b.rate.length > 2 && now - b.rate[0][0] > 1000) b.rate.shift();
  // Very long (unlimited) battles: keep the live sparkline data bounded.
  if (b.live.red.length > 6000) {
    b.live.red = b.live.red.filter((_, i) => i % 2 === 0);
    b.live.blue = b.live.blue.filter((_, i) => i % 2 === 0);
  }
  drawSpark('red');
  drawSpark('blue');
  updateScoreboard();
  $('#rateLabel').textContent = `${fmt(measuredRate())} gen/s`;
  if (frame.finished) {
    if (S.mode === 'online') onlineBattleFinished(frame);
    b.playing = false;
    b.turbo = false;
    updateBattleControls();
    if (!b.resultShown && !b.skipping) {
      b.resultShown = true;
      setTimeout(() => showResult(frame), 600);
    }
  }
  updatePhaseLabel();
}

function measuredRate() {
  const r = S.battle ? S.battle.rate : [];
  if (r.length < 2) return 0;
  const [t0, g0] = r[0], [t1, g1] = r[r.length - 1];
  return t1 > t0 ? ((g1 - g0) * 1000) / (t1 - t0) : 0;
}

// vs Bot with a longer limit than the bot was trained for: steps stop exactly
// at the trained limit once, so the standings there are known.
function capAtMark(b, query, chunk) {
  const m = b.mark;
  if (!m || m.reached || b.frame.gen >= m.gen) return query;
  const left = m.gen - b.frame.gen;
  const n = /(^|&)n=(\d+)/.exec(query);
  return n ? query.replace(/(^|&)n=\d+/, `$1n=${Math.min(+n[2], left)}`)
    : query.replace(/(^|&)ms=\d+/, `$1n=${Math.min(chunk, left)}`);
}

function requestStep(query) {
  const b = S.battle;
  const id = b.id;
  b.pending = api(`/api/step?id=${id}&${capAtMark(b, query, 100)}`)
    .then(frame => { if (S.battle && S.battle.id === id) applyFrame(frame); })
    .catch(err => { toast(err.message); if (S.battle) S.battle.playing = false; updateBattleControls(); })
    .finally(() => { if (S.battle && S.battle.id === id) S.battle.pending = null; });
  return b.pending;
}

function setPlaying(playing) {
  const b = S.battle;
  if (!b || b.frame.finished) return;
  b.playing = playing;
  if (!playing) b.turbo = false;
  b.acc = 0;
  updateBattleControls();
  updatePhaseLabel();
}

function toggleTurbo() {
  const b = S.battle;
  if (!b || b.frame.finished) return;
  b.turbo = !b.turbo;
  if (b.turbo) b.playing = true;
  updateBattleControls();
  updatePhaseLabel();
}

function stepOnce() {
  const b = S.battle;
  if (!b || b.frame.finished || b.pending) return;
  setPlaying(false);
  requestStep('n=1');
}

async function skipToEnd() {
  const b = S.battle;
  if (!b || b.frame.finished || b.skipping) return;
  b.playing = false;
  b.turbo = false;
  b.skipping = true;
  updateBattleControls();
  b.stopSkip = false;
  const dialog = $('#skipDialog');
  $('#skipProgressBar').classList.toggle('endless', !b.frame.maxGen);
  $('#skipText').textContent = '';
  dialog.showModal();
  try {
    if (b.pending) await b.pending;
    let frame = b.frame;
    while (!frame.finished && S.battle === b && !b.stopSkip) {
      frame = await api(`/api/step?id=${b.id}&${capAtMark(b, 'ms=150', 400)}&board=0`);
      b.frame = frame;
      if (b.mark && !b.mark.reached && frame.gen === b.mark.gen) Object.assign(b.mark, { reached: true, red: frame.red, blue: frame.blue });
      $('#skipProgress').style.width = frame.maxGen ? `${(frame.gen / frame.maxGen) * 100}%` : '100%';
      const of = frame.maxGen ? ` of ${fmt(frame.maxGen)}` : ' (no limit)';
      $('#skipText').textContent = `Generation ${fmt(frame.gen)}${of} · Red ${fmt(frame.red)} · Blue ${fmt(frame.blue)}`;
    }
    if (S.battle === b) {
      b.skipping = false;
      applyFrame(await api(`/api/step?id=${b.id}&n=0`));
    }
  } catch (err) {
    toast(err.message);
  } finally {
    b.skipping = false;
    dialog.close();
    updateBattleControls();
  }
}

function updateBattleControls() {
  const b = S.battle;
  if (!b) return;
  const done = b.frame.finished;
  $('#btnPlay').textContent = b.playing ? '⏸' : '▶';
  $('#btnPlay').disabled = done;
  $('#btnStep').disabled = done;
  $('#btnSkip').disabled = done;
  $('#btnTurbo').disabled = done;
  $('#btnTurbo').classList.toggle('on', b.turbo);
  $('#speedControl').classList.toggle('turbo-on', b.turbo);
  updateSpeedUi();
}

// Main loop: asks the server for as many generations as the chosen speed
// requires and redraws when something changed.
let lastTick = performance.now();
function tick(now) {
  const dt = Math.min(0.25, (now - lastTick) / 1000);
  lastTick = now;
  const b = S.battle;
  if (S.phase === 'battle' && b && b.playing && !b.skipping && !b.frame.finished) {
    if (b.turbo) {
      if (!b.pending) requestStep('ms=40');
    } else {
      b.acc = Math.min(b.acc + S.speed * dt, S.speed * 0.25 + 1);
      if (!b.pending && b.acc >= 1) {
        const n = Math.floor(b.acc);
        b.acc -= n;
        requestStep(`n=${n}`);
      }
    }
    if (b.turbo) updateSpeedUi();
  }
  if (S.dirty) render();
  requestAnimationFrame(tick);
}

// ---------------------------------------------------------------------------
// Speed dial (logarithmic, 1 .. 1000 generations per second)
// ---------------------------------------------------------------------------
const speedToPos = s => Math.log(s / SPEED_MIN) / Math.log(SPEED_MAX / SPEED_MIN);
const posToSpeed = t => SPEED_MIN * Math.pow(SPEED_MAX / SPEED_MIN, Math.max(0, Math.min(1, t)));

function setSpeed(speed) {
  S.speed = Math.max(SPEED_MIN, Math.min(SPEED_MAX, speed));
  if (S.battle && S.battle.turbo) { S.battle.turbo = false; updateBattleControls(); updatePhaseLabel(); }
  updateSpeedUi();
  savePrefs();
}

function updateSpeedUi() {
  const t = speedToPos(S.speed);
  const track = $('#speedTrack');
  track.style.setProperty('--track-w', `${track.clientWidth}px`);
  $('#speedFill').style.width = `${t * 100}%`;
  $('#speedThumb').style.left = `${t * 100}%`;
  $('#speedThumb').style.setProperty('--heat', (0.15 + t * 0.85).toFixed(2));
  track.setAttribute('aria-valuenow', Math.round(S.speed));
  const turbo = S.battle && S.battle.turbo;
  const shown = turbo ? measuredRate() : S.speed;
  $('#speedValue').textContent = turbo ? fmt(shown) : (shown < 10 ? shown.toFixed(1) : fmt(shown));
}

function initSpeedDial() {
  const track = $('#speedTrack');
  const fromEvent = e => {
    const r = track.getBoundingClientRect();
    setSpeed(posToSpeed((e.clientX - r.left) / r.width));
  };
  track.addEventListener('pointerdown', e => {
    track.setPointerCapture(e.pointerId);
    fromEvent(e);
    const move = ev => fromEvent(ev);
    const up = () => { track.removeEventListener('pointermove', move); track.removeEventListener('pointerup', up); };
    track.addEventListener('pointermove', move);
    track.addEventListener('pointerup', up);
  });
  const nudge = dir => setSpeed(posToSpeed(speedToPos(S.speed) + dir * 0.04));
  track.addEventListener('wheel', e => { e.preventDefault(); nudge(e.deltaY < 0 ? 1 : -1); }, { passive: false });
  track.addEventListener('keydown', e => {
    if (e.key === 'ArrowRight' || e.key === 'ArrowUp') { nudge(1); e.preventDefault(); e.stopPropagation(); }
    if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') { nudge(-1); e.preventDefault(); e.stopPropagation(); }
  });
  window.speedNudge = nudge;
  updateSpeedUi();
}

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------
function showResult(frame) {
  const banner = $('#resultBanner');
  banner.className = `result-banner ${frame.winner}`;
  banner.textContent = frame.winner === 'draw' ? 'Draw'
    : S.mode === 'bot' ? (isBot(frame.winner) ? 'The bot wins' : 'You win!') : `${cap(frame.winner)} wins!`;
  const loser = frame.winner === 'red' ? 'Blue' : 'Red';
  let reason = '';
  if (frame.reason === 'elimination') {
    reason = frame.winner === 'draw'
      ? `Both armies were wiped out at generation ${fmt(frame.gen)}.`
      : `${loser} was wiped out at generation ${fmt(frame.gen)}.`;
  } else if (frame.reason === 'generation_limit') {
    reason = `Generation limit of ${fmt(frame.maxGen)} reached — Red ${fmt(frame.red)} vs Blue ${fmt(frame.blue)} cells.`;
  } else if (frame.reason === 'repetition') {
    const avg = n => (Math.round(n * 10) / 10).toLocaleString('en-US');
    reason = frame.period === 1
      ? `The board froze at generation ${fmt(frame.gen)} — Red ${fmt(frame.red)} vs Blue ${fmt(frame.blue)} cells.`
      : `The board started repeating every ${frame.period} generations (at ${fmt(frame.gen)}). Average over the cycle: Red ${avg(frame.redScore)} vs Blue ${avg(frame.blueScore)}.`;
  }
  $('#resultReason').textContent = reason;
  const m = S.battle && S.battle.mark;
  $('#resultMark').hidden = !(m && m.reached && frame.gen > m.gen);
  if (!$('#resultMark').hidden) $('#resultMark').textContent = `At generation ${fmt(m.gen)}, the limit the bot was trained for: ${markStanding(m)}.`;
  const army = S.battle && S.battle.botArmy;
  $('#resultBot').hidden = !army;
  if (army) $('#resultBot').textContent = `The bot played ${botArmyText(army)}.`;
  onlineDecorateResult(frame);
  $('#resultDialog').showModal();
  drawResultChart(frame.history, m && m.reached ? m.gen : 0);
}

function drawResultChart(history, markGen) {
  const c = $('#resultChart');
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth, h = c.clientHeight;
  c.width = Math.round(w * dpr);
  c.height = Math.round(h * dpr);
  const g = c.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  if (!history) return;
  const n = history.red.length;
  const lastGen = history.gen[n - 1] || 1;
  // Scale to the armies; the grey line may leave the chart at the top.
  const max = Math.max(1, ...history.red, ...history.blue) * 1.15;
  const pad = { l: 44, r: 12, t: 12, b: 24 };
  const X = i => pad.l + (history.gen[i] / lastGen) * (w - pad.l - pad.r);
  const Y = v => h - pad.b - (v / max) * (h - pad.t - pad.b);

  g.strokeStyle = '#1e2638';
  g.fillStyle = '#8d95aa';
  g.font = '11px system-ui, sans-serif';
  g.lineWidth = 1;
  for (let k = 0; k <= 4; k++) {
    const v = (max * k) / 4;
    g.beginPath(); g.moveTo(pad.l, Y(v)); g.lineTo(w - pad.r, Y(v)); g.stroke();
    g.textAlign = 'right';
    g.fillText(fmt(v), pad.l - 6, Y(v) + 4);
  }
  g.textAlign = 'left';
  g.fillText('0', pad.l, h - 6);
  g.textAlign = 'right';
  g.fillText(`generation ${fmt(history.gen[n - 1])}`, w - pad.r, h - 6);

  const step = Math.max(1, Math.floor(n / (w * 2)));
  const plot = (values, color, width, dash) => {
    g.beginPath();
    for (let i = 0; i < n; i += step) (i ? g.lineTo(X(i), Y(values[i])) : g.moveTo(X(i), Y(values[i])));
    g.lineTo(X(n - 1), Y(values[n - 1]));
    g.setLineDash(dash);
    g.strokeStyle = color;
    g.lineWidth = width;
    g.shadowColor = color;
    g.shadowBlur = dash.length ? 0 : 6;
    g.stroke();
    g.shadowBlur = 0;
    g.setLineDash([]);
  };
  g.save();
  g.beginPath();
  g.rect(pad.l, pad.t, w - pad.l - pad.r, h - pad.t - pad.b);
  g.clip();
  plot(history.grey, 'rgba(141,150,168,0.8)', 1.5, [4, 3]);
  g.restore();
  plot(history.red, COLOR.red, 2, []);
  plot(history.blue, COLOR.blue, 2, []);
  if (markGen && markGen < lastGen) {
    const x = pad.l + (markGen / lastGen) * (w - pad.l - pad.r);
    g.strokeStyle = 'rgba(232,236,245,0.55)';
    g.lineWidth = 1;
    g.setLineDash([3, 3]);
    g.beginPath(); g.moveTo(x, pad.t); g.lineTo(x, h - pad.b); g.stroke();
    g.setLineDash([]);
    g.fillStyle = '#8d95aa';
    g.textAlign = 'center';
    g.fillText('bot trained to here', x, pad.t + 10);
  }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
function openSettings() {
  const s = S.settings;
  const locked = S.mode === 'bot';
  const sizes = S.config.sizes.slice();
  if (!sizes.some(z => z.id === s.sizeId)) sizes.push({ id: s.sizeId, name: `${s.w} x ${s.h}` });
  $('#setSize').innerHTML = sizes.map(z => `<option value="${z.id}">${z.name}</option>`).join('');
  // vs Bot: the league fixes everything but the generation limit.
  for (const el of $$('#settingsForm input, #settingsForm select')) el.disabled = locked;
  $('#setUnlimited').disabled = false;
  $('#settingsLock').hidden = !locked;
  if (locked) {
    const t = S.bot.league.settings.maxGenerations;
    $('#settingsLock').textContent = '🔒 vs Bot: only the generation limit can be changed; the rest is fixed to what the bot was trained with.' +
      (t ? ` It was trained with a limit of ${fmt(t)} generations — beyond that it plays on, but was never judged there.` : '');
  }
  $('#setSize').value = s.sizeId;
  $('#setBudget').value = s.budget;
  $('#setMaxGen').value = s.maxGen;
  $('#setUnlimited').checked = s.unlimited;
  $('#setMaxGen').disabled = s.unlimited;
  $('#setBlind').checked = s.blind;
  for (const r of $$('input[name=variant]')) r.checked = r.value === s.variant;
  onlineSettingsForm();
  $('#settingsDialog').showModal();
}

function readLimit() {
  return {
    maxGen: Math.max(10, Math.min(10000000, Math.round(+$('#setMaxGen').value || 2000))),
    unlimited: $('#setUnlimited').checked,
  };
}

function applySettings() {
  if (S.mode === 'online' && !onlineMayEditSettings()) return;
  const s = S.settings;
  if (S.mode === 'bot') {
    S.bot.limit = readLimit();
    Object.assign(s, S.bot.limit);
    savePrefs();
    if (S.phase === 'battle') resetPreparation({ clearArmies: false });
    updateModeUi();
    boardChanged();
    return;
  }
  const size = S.config.sizes.find(z => z.id === $('#setSize').value) || S.config.sizes[1];
  const sizeChanged = size.id !== s.sizeId;
  const blindChanged = $('#setBlind').checked !== s.blind;
  s.sizeId = size.id;
  s.w = size.width;
  s.h = size.height;
  s.budget = Math.max(1, Math.min(10000, Math.round(+$('#setBudget').value || 120)));
  Object.assign(s, readLimit());
  s.blind = $('#setBlind').checked;
  s.variant = ($$('input[name=variant]').find(r => r.checked) || {}).value || 'birth';
  // Drop the most recent placements of a side that no longer fits the budget.
  for (const owner of ['red', 'blue']) {
    while (remaining(owner) < 0) {
      const own = S.placements.filter(p => p.owner === owner);
      S.placements = S.placements.filter(p => p !== own[own.length - 1]);
    }
  }
  savePrefs();
  if (S.phase === 'battle') resetPreparation({ clearArmies: sizeChanged });
  else if (sizeChanged || blindChanged) resetPreparation({ clearArmies: sizeChanged });
  if (sizeChanged) {
    S.grey = null;
    off.width = 0;
    resize();
    loadBattlefield();
  }
  if (S.mode === 'online') onlineSettingsApplied();
  boardChanged();
}

// ---------------------------------------------------------------------------
// vs Bot: one player against a league trained by life_arena. The league was
// trained for one battlefield and one set of rules, so those are taken over
// unchanged. The bot draws its army before the human places anything and keeps
// it hidden until the battle starts, exactly like a blind human opponent.
// ---------------------------------------------------------------------------
function isBot(owner) { return S.mode === 'bot' && owner === S.bot.side; }
function humanSide() { return S.bot.side === 'red' ? 'blue' : 'red'; }

// The generation limit the bot was trained with, when the player has chosen to
// play on beyond it (a longer limit or none); null otherwise.
function trainedMark() {
  const t = S.mode === 'bot' && S.bot.league ? S.bot.league.settings.maxGenerations : 0;
  return t && (S.settings.unlimited || S.settings.maxGen > t) ? { gen: t, reached: false } : null;
}

function markStanding(m) {
  const lead = m.red === m.blue ? null : m.red > m.blue ? 'red' : 'blue';
  const verdict = !lead ? 'it would have been a draw' : isBot(lead) ? 'the bot would have won there' : 'you would have won there';
  return `Red ${fmt(m.red)} vs Blue ${fmt(m.blue)} cells, ${verdict}`;
}

// Why the server cannot reproduce the league's battle settings, or null.
function leagueProblem(league) {
  const s = league && league.settings;
  if (!s || !Array.isArray(league.armies)) return 'it is not a life_arena league';
  if (!S.config.presets.some(p => p.id === s.preset)) return `unknown battlefield "${s.preset}"`;
  // The ranges /api/battlefield and /api/battle accept (larger values would be clamped).
  if (!(s.width >= 50 && s.width <= 1000 && s.height >= 20 && s.height <= 600)) return `unsupported size ${s.width} x ${s.height}`;
  if (!(Number.isInteger(s.fieldSeed) && s.fieldSeed >= 0 && s.fieldSeed <= 2 ** 52)) return `unsupported battlefield seed ${s.fieldSeed}`;
  if (!(s.count >= 0 && s.count <= 200000 && s.budget >= 1 && s.maxGenerations >= 0 && s.maxGenerations <= 10000000)) return 'settings out of range';
  return null;
}

// Why a placement list would be illegal for a human on this page, or null.
// Same rules as placementFor(): every piece fully inside the own zone, and the
// total cost within the budget.
function armyProblem(list, owner, settings) {
  const { width, height, budget } = settings;
  const [zx0, zx1] = zoneRange(owner, width);
  let cost = 0;
  for (const p of list) {
    const pattern = S.arsenal.get(p.pattern);
    if (!pattern) return `unknown pattern ${p.pattern}`;
    if (![p.x, p.y, p.rot].every(Number.isInteger)) return `bad placement of ${p.pattern}`;
    const shape = oriented(pattern, { rot: p.rot & 3, mirror: !!p.mirror });
    if (p.x < zx0 || p.x + shape.w > zx1 || p.y < 0 || p.y + shape.h > height) return `${p.pattern} at ${p.x},${p.y} is outside the ${owner} zone`;
    cost += pattern.cost;
  }
  return cost > budget ? `it costs ${cost} of ${budget} points` : null;
}

// A league placement list as page placements: (x, y) is the top-left corner of
// the piece after `rot` clockwise quarter turns and then the optional mirror,
// the same convention as gol::transformed() and Arena::board().
function botPlacements(list, owner) {
  return list.map(p => {
    const pattern = S.arsenal.get(p.pattern);
    const shape = oriented(pattern, { rot: p.rot & 3, mirror: !!p.mirror });
    return { id: 0, owner, patternId: pattern.id, cost: pattern.cost, x: p.x, y: p.y, w: shape.w, h: shape.h, cells: shape.cells };
  });
}

// The bot's army for the next battle. Hard follows the league's Nash mixture,
// Easy picks any of its armies with equal chance. Nothing the human does
// influences the draw.
function pickBotArmy() {
  const armies = S.bot.league ? S.bot.league.playable : [];
  const weight = a => (S.bot.level === 'hard' ? Math.max(0, +a.weight || 0) : 1);
  const candidates = armies.filter(a => weight(a) > 0);
  if (!candidates.length) return armies.length ? armies[Math.floor(Math.random() * armies.length)] : null;
  let r = Math.random() * candidates.reduce((sum, a) => sum + weight(a), 0);
  for (const a of candidates) {
    r -= weight(a);
    if (r < 0) return a;
  }
  return candidates[candidates.length - 1];
}

function botArmyText(army) {
  const n = S.bot.league ? S.bot.league.armies.length : 0;
  return `army #${army.id} of ${n} (${army.origin}, ${army.cost} pts, Nash weight ${Math.round(army.weight * 1000) / 10}%)`;
}

// Switches to vs Bot mode with league `id` (the first one if unknown). Returns false if that is not possible.
async function useBot(id) {
  const entry = S.bots.find(b => b.id === id) || S.bots[0];
  if (!entry) return false;
  let league;
  try {
    league = await api(`/api/bot?id=${encodeURIComponent(entry.id)}`);
  } catch (err) {
    toast(`Could not load the bot: ${err.message}`);
    return false;
  }
  const problem = leagueProblem(league);
  if (problem) {
    toast(`The bot ${entry.id} cannot be played here: ${problem}.`);
    return false;
  }
  // Only armies that are legal on this page, for both sides, are ever played.
  league.playable = league.armies.filter(a => {
    const why = armyProblem(a.red, 'red', league.settings) || armyProblem(a.blue, 'blue', league.settings);
    if (why) console.warn(`bot ${entry.id}: army #${a.id} is never played: ${why}`);
    return !why;
  });
  if (!league.playable.length) {
    toast(`The bot ${entry.id} has no legal army.`);
    return false;
  }

  if (S.mode !== 'bot') S.duo = { settings: { ...S.settings }, field: { ...S.field } };
  const ls = league.settings;
  const size = S.config.sizes.find(z => z.width === ls.width && z.height === ls.height);
  Object.assign(S.settings, {
    sizeId: size ? size.id : 'league', w: ls.width, h: ls.height, budget: ls.budget,
    maxGen: ls.maxGenerations || S.settings.maxGen, unlimited: ls.maxGenerations === 0, ...S.bot.limit,
    variant: ls.variant === 'contagion' ? 'contagion' : 'birth', blind: false,
  });
  S.field = { preset: ls.preset, count: ls.count, seed: ls.fieldSeed };
  S.mode = 'bot';
  S.bot.id = entry.id;
  S.bot.league = league;
  await rebuildBoard();
  return true;
}

async function setMode(mode) {
  if (mode === S.mode) return;
  const wasOnline = S.mode === 'online';
  if (wasOnline) onlineLeave();  // back to the two-player settings
  if (mode === 'online') return onlineStart();
  if (mode === 'bot') {
    if (!(await useBot(S.bot.id)) && wasOnline) await rebuildBoard();
    return;
  }
  if (S.duo) {
    Object.assign(S.settings, S.duo.settings);
    S.field = { ...S.duo.field };
  }
  S.duo = null;
  S.mode = 'duo';
  S.bot.army = null;
  await rebuildBoard();
}

// After a change of mode or league: new size, new battlefield, empty armies.
function rebuildBoard() {
  S.battle = null;
  S.grey = null;
  off.width = 0;
  for (const d of $$('dialog[open]')) d.close();
  updateModeUi();
  syncFieldControls();
  resize();
  resetPreparation({ clearArmies: true });
  savePrefs();
  return loadBattlefield();
}

function setBotSide(human) {
  const side = human === 'red' ? 'blue' : 'red';
  if (side === S.bot.side || S.phase !== 'prep') return;
  S.bot.side = side;
  updateModeUi();
  resetPreparation({ clearArmies: true });
  savePrefs();
}

function setBotLevel(level) {
  if (level === S.bot.level) return;
  S.bot.level = level;
  S.bot.army = pickBotArmy();
  updateModeUi();
  savePrefs();
}

function updateModeUi() {
  const bot = S.mode === 'bot';
  $('#modeSwitch').hidden = false;
  $('#modeSwitch [data-mode=bot]').hidden = !S.bots.length;
  for (const b of $$('#modeSwitch button')) b.classList.toggle('on', b.dataset.mode === S.mode);
  for (const sel of ['#presetSelect', '#btnReroll', '#countRow', '#presetDesc']) $(sel).hidden = bot;
  $('#botPrep').hidden = !bot;
  for (const owner of ['red', 'blue']) {
    $(`.score.${owner} .label`).textContent = bot ? `${cap(owner)} · ${isBot(owner) ? 'bot' : 'you'}` : cap(owner);
  }
  updateOnlineUi();
  if (!bot) return;
  const s = S.settings;
  const preset = S.config.presets.find(p => p.id === S.field.preset);
  $('#botField').textContent = `🔒 ${preset.name}${preset.usesCount ? ` · ${fmt(S.field.count)} grey` : ''} · seed ${S.field.seed}`;
  $('#botField').title = `${s.w} x ${s.h} board, ${s.budget} points per player, ` +
    `${s.unlimited ? 'no generation limit' : `limit ${fmt(s.maxGen)} generations`}` +
    `${S.bot.league.settings.maxGenerations ? ` (trained with ${fmt(S.bot.league.settings.maxGenerations)})` : ''}, ` +
    `${s.variant === 'contagion' ? 'Contagion' : 'Birthright'} rule. Only the generation limit can be changed (⚙).`;
  const select = $('#botSelect');
  select.hidden = S.bots.length < 2;
  select.innerHTML = S.bots.map(b => `<option value="${b.id}">${b.id}</option>`).join('');
  select.value = S.bot.id;
  for (const b of $$('#botPrep [data-side]')) b.classList.toggle('on', b.dataset.side === humanSide());
  for (const b of $$('#botPrep [data-level]')) b.classList.toggle('on', b.dataset.level === S.bot.level);
}

// During the battle the bot's army is no secret any more.
function updateBotInfo() {
  const army = S.battle && S.battle.botArmy;
  $('#botInfo').hidden = !army;
  if (army) {
    $('#botInfo').textContent = `Bot: army #${army.id} · ${army.origin}`;
    $('#botInfo').title = `The bot played ${botArmyText(army)}.`;
  }
}

// ---------------------------------------------------------------------------
// Focus mode: once both armies are set, the board can take the whole screen.
// Scoreboard and controls float above it and fade out while the mouse rests.
// ---------------------------------------------------------------------------
const focusAvailable = () => S.phase === 'battle' || (S.ready.red && S.ready.blue);

function updateFocusAvailability() {
  const ok = focusAvailable();
  $('#btnFocus').disabled = !ok;
  if (!ok && S.focus) setFocus(false);
}

function setFocus(on) {
  if (on && !focusAvailable()) return;
  if (on === S.focus) return;
  S.focus = on;
  document.body.classList.toggle('focus', on);
  document.body.classList.remove('idle');
  $('#btnFocus').title = on ? 'Leave focus mode (F / Esc)' : 'Focus mode: maximise the board (F)';
  if (on) {
    const el = document.documentElement;
    if (el.requestFullscreen && !document.fullscreenElement) el.requestFullscreen().catch(() => { /* windowed focus is fine too */ });
    armIdle();
  } else if (document.fullscreenElement) {
    document.exitFullscreen().catch(() => {});
  }
}

let idleTimer = 0;
function armIdle() {
  document.body.classList.remove('idle');
  clearTimeout(idleTimer);
  if (!S.focus) return;
  idleTimer = setTimeout(() => {
    const busy = document.querySelector('.controlbar:hover, .scoreboard:hover, dialog[open]');
    if (busy) armIdle();
    else document.body.classList.add('idle');
  }, 2500);
}

function newGame() {
  if (S.mode === 'online') return onlineNewGame();
  if (S.battle) S.battle = null;
  if (S.mode !== 'bot' && ['random', 'random_mirror', 'random_rotational', 'reactor'].includes(S.field.preset)) {
    S.field.seed = newSeed();
    loadBattlefield();
  }
  resetPreparation({ clearArmies: true });
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------
function wire() {
  for (const owner of ['red', 'blue']) {
    $(`#side${cap(owner)}`).addEventListener('click', e => {
      const btn = e.target.closest('button[data-act]');
      if (btn) onSideAction(owner, btn.dataset.act);
    });
  }

  const presetSelect = $('#presetSelect');
  presetSelect.innerHTML = S.config.presets.map(p => `<option value="${p.id}">${p.name}</option>`).join('');
  presetSelect.addEventListener('change', () => {
    S.field.preset = presetSelect.value;
    syncFieldControls();
    loadBattlefield();
    savePrefs();
  });
  $('#btnReroll').addEventListener('click', () => { S.field.seed = newSeed(); loadBattlefield(); });
  $('#countRange').addEventListener('input', e => setCount(e.target.value));
  $('#countInput').addEventListener('change', e => setCount(e.target.value));
  $('#btnStart').addEventListener('click', () => startBattle());

  $('#btnPlay').addEventListener('click', () => S.battle && setPlaying(!S.battle.playing));
  $('#btnStep').addEventListener('click', stepOnce);
  $('#btnTurbo').addEventListener('click', toggleTurbo);
  $('#btnSkip').addEventListener('click', skipToEnd);
  $('#btnRestart').addEventListener('click', () => S.battle && startBattle(S.battle.encoded));
  $('#btnSkipStop').addEventListener('click', () => { if (S.battle) S.battle.stopSkip = true; });
  $('#skipDialog').addEventListener('cancel', e => { e.preventDefault(); if (S.battle) S.battle.stopSkip = true; });
  $('#btnFocus').addEventListener('click', () => setFocus(!S.focus));
  for (const owner of ['red', 'blue']) $(`#tab${cap(owner)}`).addEventListener('click', () => openDrawer(owner));
  document.addEventListener('fullscreenchange', () => { if (!document.fullscreenElement) setFocus(false); });
  document.addEventListener('pointermove', armIdle);
  $('#setUnlimited').addEventListener('change', e => { $('#setMaxGen').disabled = e.target.checked; });
  $('#btnBackToPrep').addEventListener('click', () => { S.battle = null; resetPreparation({ clearArmies: false }); });

  $('#btnResultView').addEventListener('click', () => $('#resultDialog').close());
  $('#btnResultReplay').addEventListener('click', () => { $('#resultDialog').close(); startBattle(S.battle.encoded); });
  $('#btnResultEdit').addEventListener('click', () => { $('#resultDialog').close(); S.battle = null; resetPreparation({ clearArmies: false }); });
  $('#btnResultNew').addEventListener('click', () => { $('#resultDialog').close(); newGame(); });

  $('#btnSettings').addEventListener('click', openSettings);
  $('#settingsDialog').addEventListener('close', () => {
    if ($('#settingsDialog').returnValue === 'apply') applySettings();
  });
  $('#btnRules').addEventListener('click', () => $('#rulesDialog').showModal());
  $('#btnRulesClose').addEventListener('click', () => $('#rulesDialog').close());
  $('#btnNew').addEventListener('click', newGame);

  for (const btn of $$('#modeSwitch button')) btn.addEventListener('click', () => setMode(btn.dataset.mode));
  for (const btn of $$('#botPrep [data-side]')) btn.addEventListener('click', () => setBotSide(btn.dataset.side));
  for (const btn of $$('#botPrep [data-level]')) btn.addEventListener('click', () => setBotLevel(btn.dataset.level));
  $('#botSelect').addEventListener('change', e => useBot(e.target.value));
  wireOnline();

  new ResizeObserver(() => {
    resize();
    if (S.battle) { drawSpark('red'); drawSpark('blue'); }
    updateSpeedUi();
  }).observe($('#stage'));
  document.addEventListener('keydown', onKey);
}

function onKey(e) {
  if (document.querySelector('dialog[open]')) return;
  if (e.target.matches('input, select, textarea')) return;
  const key = e.key;
  if (key === 'h' || key === '?') { $('#rulesDialog').showModal(); return; }
  const isF = key === 'f' || key === 'F';

  if (S.phase === 'prep') {
    if (key === 'Escape' && S.drag) endDrag();
    else if (key === 'Escape' && S.focus) setFocus(false);
    else if (isF && focusAvailable()) setFocus(!S.focus);
    else if (key === 'a' || key === 'A') toggleDrawer();
    else if (key === 'r' || key === 'R') changeOrientation(focusOwner(), e.shiftKey ? 'rotl' : 'rotr');
    else if (isF) changeOrientation(focusOwner(), 'mirror');
    else if (key === 'v' || key === 'V') changeOrientation(focusOwner(), 'vflip');
    else if ((e.ctrlKey || e.metaKey) && key === 'z') { const o = focusOwner(); if (o) onSideAction(o, 'undo'); }
    else return;
    e.preventDefault();
    return;
  }

  if (!S.battle) return;
  if (key === ' ') setPlaying(!S.battle.playing);
  else if (key === 'ArrowRight' || key === 'n') stepOnce();
  else if (key === '+' || key === '=') window.speedNudge(1);
  else if (key === '-' || key === '_') window.speedNudge(-1);
  else if (key === 'm' || key === 'M') toggleTurbo();
  else if (isF) setFocus(!S.focus);
  else if (key === 'Escape' && S.focus) setFocus(false);
  else return;
  e.preventDefault();
}

async function init() {
  loadPrefs();
  try {
    S.config = await api('/api/config');
  } catch (err) {
    $('#phaseLabel').textContent = window.lifeDuelBackend ? 'The battle engine could not start.' : 'Cannot reach the life_battle server.';
    toast(err.message);
    return;
  }
  for (const p of S.config.arsenal) S.arsenal.set(p.id, p);
  const size = S.config.sizes.find(z => z.id === S.settings.sizeId) || S.config.sizes[1];
  Object.assign(S.settings, { sizeId: size.id, w: size.width, h: size.height });

  try {
    S.bots = (await api('/api/bots')).bots;
  } catch { S.bots = []; }

  buildCards();
  wire();
  initSpeedDial();
  const startInBotMode = S.mode === 'bot' && S.bots.length > 0;
  S.mode = 'duo';
  updateModeUi();
  syncFieldControls();
  resize();
  resetPreparation({ clearArmies: true });
  if (!startInBotMode || !(await useBot(S.bot.id))) await loadBattlefield();
  requestAnimationFrame(tick);
  onlineJoinFromLink();

  let seenRules = false;
  try { seenRules = localStorage.getItem('lifeDuel.seenRules') === '1'; localStorage.setItem('lifeDuel.seenRules', '1'); } catch { /* ignore */ }
  if (!seenRules) $('#rulesDialog').showModal();
}

init();
