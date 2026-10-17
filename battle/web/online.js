// Life Duel - online play against a friend on another computer.
//
// The host gets a link (the page URL with #join=<peer id>); the friend opens it
// and the two pages talk directly over WebRTC (PeerJS, its public signalling
// server only brokers the connection). There is no game server: both pages
// simulate the battle themselves with the same deterministic engine.
//
// A round: the host sends the settings (setup). Each player places an army
// that the other must not see, so "Ready" sends only a commitment, the SHA-256
// of the army and a random salt. Once both are committed, both reveal; each
// page checks the reveal against the commitment and the placement rules, builds
// the same starting board and runs the battle. Checksums of the starting board
// and of the final result (with its whole history) are compared to detect any
// desynchronisation. After a battle either player can ask for a rematch; when
// both agree the sides swap.
//
// Loaded before app.js; uses its globals (S, $, toast, ...) only when called.
'use strict';

const ONLINE_PROTOCOL = 1;
const PEERJS_URL = 'https://unpkg.com/peerjs@1.5.4/dist/peerjs.min.js';
const ONLINE_PING_MS = 3000;
const ONLINE_LOST_MS = 20000;
const ONLINE_JOIN_TIMEOUT_MS = 25000;
const JOIN_PREFIX = '#join=';

function freshNet(role) {
  return {
    role,                 // 'host' | 'guest'
    status: 'off',        // 'creating' | 'waiting' | 'connecting' | 'connected' | 'lost' | 'failed'
    problem: '',          // why the connection failed or was lost
    link: '',
    peer: null,
    conn: null,
    pingTimer: 0,
    joinTimer: 0,
    lastSeen: 0,
    hostSide: 'red',      // the host's colour this round; swaps on every rematch
    round: 0,             // 0: no round yet (nobody connected)
    setupKey: '',         // settings + battlefield of the round, to notice changes by the host
    fieldRound: 0,        // the round whose battlefield has been loaded
    mine: null,           // { army, salt, hash, revealed }
    theirs: { hash: null, army: null },
    started: false,       // this round's battle has been started
    sums: { start: null, theirStart: null, final: null, theirFinal: null },
    desync: '',
    rematch: 'none',      // 'none' | 'requested' (by this page) | 'offered' (by the friend)
  };
}

let NET = freshNet(null);

const otherSide = side => (side === 'red' ? 'blue' : 'red');
function mySide() { return NET.role === 'guest' ? otherSide(NET.hostSide) : NET.hostSide; }
function isRemote(owner) { return S.mode === 'online' && owner !== mySide(); }
const onlineConnected = () => NET.status === 'connected';

// ---------------------------------------------------------------------------
// SHA-256 (synchronous, also where crypto.subtle is missing: plain http on a LAN)
// ---------------------------------------------------------------------------
const SHA_K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

function sha256(text) {
  const data = new TextEncoder().encode(text);
  const len = data.length;
  const blocks = Math.ceil((len + 9) / 64);
  const msg = new Uint8Array(blocks * 64);
  msg.set(data);
  msg[len] = 0x80;
  const view = new DataView(msg.buffer);
  view.setUint32(msg.length - 8, Math.floor(len / 0x20000000));
  view.setUint32(msg.length - 4, (len * 8) >>> 0);
  const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const w = new Uint32Array(64);
  const rotr = (x, n) => (x >>> n) | (x << (32 - n));
  for (let b = 0; b < blocks; b++) {
    for (let i = 0; i < 16; i++) w[i] = view.getUint32(b * 64 + i * 4);
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
      const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    let [a, bb, c, d, e, f, g, hh] = h;
    for (let i = 0; i < 64; i++) {
      const t1 = (hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i]) >>> 0;
      const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c))) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = bb; bb = a; a = (t1 + t2) >>> 0;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  return [...h].map(x => x.toString(16).padStart(8, '0')).join('');
}

function randomHex(bytes) {
  const a = new Uint8Array(bytes);
  crypto.getRandomValues(a);
  return [...a].map(x => x.toString(16).padStart(2, '0')).join('');
}

// What a player commits to. Canonical, so both pages hash the same text.
function commitText(round, side, army, salt) {
  const list = army.map(p => ({ pattern: String(p.pattern), x: p.x, y: p.y, rot: p.rot, mirror: !!p.mirror }));
  return JSON.stringify({ game: 'life-duel', round, side, army: list, salt: String(salt) });
}

// ---------------------------------------------------------------------------
// Connection (PeerJS)
// ---------------------------------------------------------------------------
let peerJsPromise = null;
function loadPeerJs() {
  const found = () => window.Peer || (window.peerjs && window.peerjs.Peer);
  if (found()) return Promise.resolve(found());
  peerJsPromise = peerJsPromise || new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = PEERJS_URL;
    s.onload = () => (found() ? resolve(found()) : reject(new Error('PeerJS did not load')));
    s.onerror = () => { peerJsPromise = null; reject(new Error('Could not load PeerJS - are you offline?')); };
    document.head.appendChild(s);
  });
  return peerJsPromise;
}

// Optional PeerJS options (own signalling server, TURN servers): window.LIFE_DUEL_PEER_OPTIONS.
const peerOptions = () => window.LIFE_DUEL_PEER_OPTIONS || {};

function setStatus(status, problem = '') {
  NET.status = status;
  NET.problem = problem;
  updateOnlineUi();
  updatePhaseLabel();
}

async function hostGame() {
  const net = NET;
  setStatus('creating');
  let Peer;
  try {
    Peer = await loadPeerJs();
  } catch (err) {
    if (NET === net) setStatus('failed', err.message);
    return;
  }
  if (NET !== net) return;
  const peer = new Peer(peerOptions());
  net.peer = peer;
  let reconnects = 0;
  peer.on('open', id => {
    if (NET !== net) return;
    reconnects = 0;
    if (net.conn) return;
    net.link = `${location.origin}${location.pathname}${location.search}${JOIN_PREFIX}${encodeURIComponent(id)}`;
    setStatus('waiting');
  });
  peer.on('connection', conn => {
    conn.on('open', () => {
      if (NET !== net || net.conn) { conn.close(); return; }  // one friend per game
      attach(net, conn);
    });
  });
  peer.on('error', err => {
    if (NET !== net || net.conn) return;
    setStatus('failed', `Could not create an online game: ${err.message || err.type}`);
  });
  peer.on('disconnected', () => {
    // Lost the signalling server while waiting for the friend: try again a few times.
    if (NET !== net || net.conn || peer.destroyed) return;
    if (reconnects++ < 5) setTimeout(() => { if (NET === net && !net.conn && peer.disconnected && !peer.destroyed) peer.reconnect(); }, 1000 * reconnects);
    else setStatus('failed', 'Lost the connection to the PeerJS server. Try a new link.');
  });
}

async function joinGame(id) {
  const net = NET;
  setStatus('connecting');
  net.joinTimer = setTimeout(() => {
    if (NET === net && !net.conn) fail(net, 'Could not reach the game behind this link. Ask your friend for a new one.');
  }, ONLINE_JOIN_TIMEOUT_MS);
  let Peer;
  try {
    Peer = await loadPeerJs();
  } catch (err) {
    fail(net, err.message);
    return;
  }
  if (NET !== net) return;
  const peer = new Peer(peerOptions());
  net.peer = peer;
  peer.on('open', () => {
    if (NET !== net) return;
    const conn = peer.connect(id, { reliable: true, serialization: 'json' });
    conn.on('open', () => {
      if (NET !== net || net.conn) { conn.close(); return; }
      attach(net, conn);
    });
    conn.on('error', err => fail(net, `Could not connect: ${err.message || err.type}`));
  });
  peer.on('error', err => {
    if (net.conn) return;
    fail(net, err.type === 'peer-unavailable'
      ? 'No game found for this link - it is used up or the host has left. Ask your friend for a new one.'
      : `Could not connect: ${err.message || err.type}`);
  });
}

function fail(net, message) {
  if (NET !== net || net.status === 'failed') return;
  closeConnection(net);
  setStatus('failed', message);
}

function attach(net, conn) {
  clearTimeout(net.joinTimer);
  net.conn = conn;
  // The signalling server is not needed any more; this also retires the link.
  try { net.peer.disconnect(); } catch { /* ignore */ }
  conn.on('data', data => { if (NET === net) onMessage(data); });
  conn.on('close', () => { if (NET === net) connectionLost('Your friend has left the game.'); });
  conn.on('error', () => { if (NET === net) connectionLost('The connection to your friend failed.'); });
  net.lastSeen = Date.now();
  net.pingTimer = setInterval(() => {
    if (NET !== net) return;
    send({ t: 'ping' });
    if (Date.now() - net.lastSeen > ONLINE_LOST_MS) connectionLost('No answer from your friend for a while.');
  }, ONLINE_PING_MS);
  send({ t: 'hello', v: ONLINE_PROTOCOL });
}

function send(message) {
  if (NET.conn && NET.conn.open) {
    try { NET.conn.send(message); } catch { /* reported by the close/error events */ }
  }
}

function closeConnection(net) {
  clearInterval(net.pingTimer);
  clearTimeout(net.joinTimer);
  const { conn, peer } = net;
  net.conn = null;
  net.peer = null;
  try { if (conn) conn.close(); } catch { /* ignore */ }
  // Give the close a moment to reach the friend before tearing everything down.
  setTimeout(() => { try { if (peer) peer.destroy(); } catch { /* ignore */ } }, 500);
}

function connectionLost(message) {
  if (NET.status === 'lost' || NET.status === 'failed' || NET.status === 'off') return;
  closeConnection(NET);
  setStatus('lost', message);
  toast(message);
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------
function onMessage(m) {
  if (!m || typeof m !== 'object') return;
  NET.lastSeen = Date.now();
  switch (m.t) {
    case 'hello':
      if (m.v !== ONLINE_PROTOCOL) {
        closeConnection(NET);
        setStatus('failed', 'Your friend runs another version of Life Duel. Both of you: reload the page.');
        return;
      }
      setStatus('connected');
      toast(NET.role === 'host' ? 'Your friend has joined!' : 'Connected to your friend.');
      if (NET.role === 'host') sendSetup();
      return;
    case 'setup':
      if (NET.role === 'guest') applySetup(m);
      return;
    case 'commit':
      if (m.round !== NET.round || typeof m.hash !== 'string') return;
      NET.theirs.hash = m.hash;
      S.ready[otherSide(mySide())] = true;
      refresh();
      maybeReveal();
      return;
    case 'reveal':
      if (m.round === NET.round) onReveal(m);
      return;
    case 'start':
      if (m.round !== NET.round) return;
      NET.sums.theirStart = m.sum;
      compareSums();
      return;
    case 'final':
      if (m.round !== NET.round) return;
      NET.sums.theirFinal = m.sum;
      compareSums();
      return;
    case 'rematch':
      if (NET.rematch === 'requested') {
        if (NET.role === 'host') startRematch();
      } else {
        NET.rematch = 'offered';
        toast('Your friend wants a rematch.');
      }
      refresh();
      return;
    case 'bye':
      connectionLost('Your friend has left the game.');
      return;
    default:
  }
}

// The settings of a round, as the host sends them.
function currentSetup() {
  const s = S.settings;
  return {
    settings: { sizeId: s.sizeId, w: s.w, h: s.h, budget: s.budget, maxGen: s.maxGen, unlimited: s.unlimited, variant: s.variant },
    field: { preset: S.field.preset, count: S.field.count, seed: S.field.seed },
  };
}

function resetRound() {
  NET.mine = null;
  NET.theirs = { hash: null, army: null };
  NET.started = false;
  NET.sums = { start: null, theirStart: null, final: null, theirFinal: null };
  NET.desync = '';
  NET.rematch = 'none';
}

function sendSetup() {
  NET.round++;
  resetRound();
  const setup = currentSetup();
  NET.setupKey = JSON.stringify(setup);
  if (S.grey) NET.fieldRound = NET.round;  // the host's battlefield is already loaded
  send({ t: 'setup', round: NET.round, hostSide: NET.hostSide, ...setup });
}

function applySetup(m) {
  const st = m.settings || {}, f = m.field || {};
  const int = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
  if (!int(m.round, 1, 1e9) || !int(st.w, 50, 1000) || !int(st.h, 20, 600) || !int(st.budget, 1, 10000) ||
      !int(st.maxGen, 0, 10000000) || !int(f.count, 0, 200000) || !int(f.seed, 0, 2 ** 52) ||
      !S.config.presets.some(p => p.id === f.preset)) {
    connectionLost('Your friend sent settings this page does not understand.');
    return;
  }
  const first = NET.round === 0;
  const swapped = (m.hostSide === 'blue' ? 'blue' : 'red') !== NET.hostSide;
  NET.round = m.round;
  NET.hostSide = m.hostSide === 'blue' ? 'blue' : 'red';
  resetRound();
  const size = S.config.sizes.find(z => z.width === st.w && z.height === st.h);
  Object.assign(S.settings, {
    sizeId: size ? size.id : 'online', w: st.w, h: st.h, budget: st.budget, maxGen: Math.max(10, st.maxGen),
    unlimited: !!st.unlimited, variant: st.variant === 'contagion' ? 'contagion' : 'birth', blind: false,
  });
  S.field = { preset: f.preset, count: f.count, seed: f.seed };
  NET.setupKey = JSON.stringify(currentSetup());
  if (!first) toast(swapped ? `Rematch: you play ${cap(mySide())}.` : 'The host changed the settings - place your army again.');
  rebuildBoard();
}

// Host: after a change of the settings or the battlefield, start the round over.
function syncSetup() {
  if (S.mode !== 'online' || NET.role !== 'host' || !onlineConnected()) return;
  if (JSON.stringify(currentSetup()) === NET.setupKey) return;
  sendSetup();
  S.battle = null;
  resetPreparation({ clearArmies: false });
}

function onReveal(m) {
  const side = otherSide(mySide());
  if (!NET.theirs.hash || NET.theirs.army) return;
  const army = Array.isArray(m.army) ? m.army.filter(p => p && typeof p === 'object') : null;
  if (!army || army.length !== m.army.length || sha256(commitText(m.round, side, army, m.salt)) !== NET.theirs.hash) {
    NET.desync = 'Your friend revealed another army than they had committed to.';
    toast(NET.desync);
    refresh();
    return;
  }
  const problem = armyProblem(army, side, { width: S.settings.w, height: S.settings.h, budget: S.settings.budget });
  if (problem) {
    NET.desync = `Your friend's army breaks the rules: ${problem}.`;
    toast(NET.desync);
    refresh();
    return;
  }
  NET.theirs.army = army;
  maybeStart();
}

function maybeReveal() {
  const mine = NET.mine;
  if (!mine || !NET.theirs.hash || mine.revealed) return;
  mine.revealed = true;
  send({ t: 'reveal', round: NET.round, army: mine.army, salt: mine.salt });
  maybeStart();
}

function maybeStart() {
  if (NET.started || !NET.mine || !NET.mine.revealed || !NET.theirs.army) return;
  if (!S.grey || NET.fieldRound !== NET.round) return;  // onlineFieldLoaded() comes back here
  NET.started = true;
  startBattle();
}

function compareSums() {
  const { start, theirStart, final, theirFinal } = NET.sums;
  if (start && theirStart && start !== theirStart) {
    NET.desync = 'Out of sync: your friend\'s page built another starting board.';
  } else if (final && theirFinal && final !== theirFinal) {
    NET.desync = 'Out of sync: your friend\'s page computed another result for this battle.';
  } else {
    refresh();
    return;
  }
  toast(NET.desync);
  refresh();
}

function requestRematch() {
  if (!onlineConnected()) return toast('Your friend is not connected any more.');
  if (NET.rematch === 'requested') return;
  send({ t: 'rematch' });
  if (NET.rematch === 'offered' && NET.role === 'host') startRematch();
  else NET.rematch = 'requested';
  refresh();
}

function startRematch() {
  NET.hostSide = otherSide(NET.hostSide);
  sendSetup();
  toast(`Rematch: you play ${cap(mySide())}.`);
  rebuildBoard();
}

// ---------------------------------------------------------------------------
// Hooks called by app.js
// ---------------------------------------------------------------------------
function stashDuoSettings() {
  if ((S.mode === 'bot' || S.mode === 'watch') && S.duo) {
    Object.assign(S.settings, S.duo.settings);
    S.field = { ...S.duo.field };
  } else if (S.mode === 'duo') {
    S.duo = { settings: { ...S.settings }, field: { ...S.field } };
  }
  S.bot.army = null;
}

// setMode('online'): host a new game with the current two-player settings.
async function onlineStart() {
  stashDuoSettings();
  S.mode = 'online';
  NET = freshNet('host');
  S.settings.blind = false;
  await rebuildBoard();
  hostGame();
}

// The page was opened with an invitation link.
function onlineJoinFromLink() {
  const hash = location.hash;
  if (!hash.startsWith(JOIN_PREFIX)) return false;
  let id = '';
  try { id = decodeURIComponent(hash.slice(JOIN_PREFIX.length)); } catch { /* bad link */ }
  // The link is single-use: a reload should not try it again.
  try { history.replaceState(null, '', location.pathname + location.search); } catch { /* ignore */ }
  if (!id) return false;
  stashDuoSettings();
  S.mode = 'online';
  NET = freshNet('guest');
  S.settings.blind = false;
  rebuildBoard();
  joinGame(id);
  return true;
}

// Leaving online mode (setMode): back to the two-player settings.
function onlineLeave() {
  if (onlineConnected()) send({ t: 'bye' });
  closeConnection(NET);
  NET = freshNet(null);
  if (S.duo) {
    Object.assign(S.settings, S.duo.settings);
    S.field = { ...S.duo.field };
  }
  S.duo = null;
  S.mode = 'duo';
}

function onlineNewLink() {
  if (onlineConnected()) send({ t: 'bye' });
  closeConnection(NET);
  NET = freshNet('host');
  S.battle = null;
  rebuildBoard();
  hostGame();
}

// "Ready": commit to the army, without showing it.
function onlineSetReady(owner) {
  if (owner !== mySide() || S.ready[owner] || S.phase !== 'prep') return;
  if (!onlineConnected() || !NET.round) return toast(NET.role === 'host' ? 'Wait until your friend has opened the link.' : 'Not connected yet.');
  if (NET.fieldRound !== NET.round) return toast('The battlefield is still loading.');
  const army = S.placements.filter(p => p.owner === owner)
    .map(p => ({ pattern: p.patternId, x: p.x, y: p.y, rot: p.rot || 0, mirror: !!p.mirror }));
  if (!army.length && !confirm('You have not placed anything and would lose immediately. Lock in anyway?')) return;
  if (S.drag) endDrag();
  const salt = randomHex(16);
  NET.mine = { army, salt, hash: sha256(commitText(NET.round, owner, army, salt)), revealed: false };
  send({ t: 'commit', round: NET.round, hash: NET.mine.hash });
  S.ready[owner] = true;
  S.open = null;
  refresh();
  maybeReveal();
}

function onlineResetPreparation() {
  const me = mySide();
  S.ready = { red: false, blue: false };
  S.ready[me] = !!NET.mine;
  S.ready[otherSide(me)] = !!NET.theirs.hash;
  S.turn = 'done';
  S.open = NET.mine ? null : me;
  updateOnlineUi();
}

function onlineOpponentPlacements() {
  return NET.theirs.army ? botPlacements(NET.theirs.army, otherSide(mySide())) : [];
}

function onlineBattleStarted(encoded) {
  S.battle.onlineRound = NET.round;
  NET.sums.start = sha256(encoded);
  send({ t: 'start', round: NET.round, sum: NET.sums.start });
  compareSums();
  updateOnlineUi();
}

function onlineBattleFinished(frame) {
  const b = S.battle;
  if (!b || b.onlineRound !== NET.round || NET.sums.final) return;
  const { gen, maxGen, red, blue, grey, winner, reason, period, redScore, blueScore, history } = frame;
  NET.sums.final = sha256(JSON.stringify({ gen, maxGen, red, blue, grey, winner, reason, period, redScore, blueScore, history }));
  send({ t: 'final', round: NET.round, sum: NET.sums.final });
  compareSums();
}

// loadBattlefield() has a new battlefield in S.grey.
function onlineFieldLoaded() {
  if (S.mode !== 'online') return;
  if (NET.role === 'guest' || !onlineConnected()) NET.fieldRound = NET.round;
  else if (JSON.stringify(currentSetup()) === NET.setupKey) NET.fieldRound = NET.round;
  else syncSetup();
  maybeStart();
}

const onlineMayEditSettings = () => NET.role === 'host' && !NET.mine && S.phase === 'prep';

function onlineSettingsForm() {
  const on = S.mode === 'online';
  $('#settingsForm button[value=apply]').hidden = false;
  if (!on) return;
  const editable = onlineMayEditSettings();
  for (const el of $$('#settingsForm input, #settingsForm select')) el.disabled = !editable;
  $('#setBlind').disabled = true;
  $('#setBlind').checked = true;
  if (editable) $('#setMaxGen').disabled = $('#setUnlimited').checked;
  $('#settingsLock').hidden = editable;
  $('#settingsLock').textContent = NET.role === 'guest'
    ? '🔒 Online: the host chooses the settings.'
    : '🔒 Your army is locked in - the settings can be changed in the next round.';
  $('#settingsForm button[value=apply]').hidden = !editable;
}

function onlineSettingsApplied() {
  S.settings.blind = false;
  syncSetup();
}

function onlineNewGame() {
  if (S.phase === 'battle') return requestRematch();
  if (NET.mine) return toast('Your army is locked in.');
  S.placements = S.placements.filter(p => p.owner !== mySide());
  boardChanged();
}

function onlinePhaseLabel() {
  const you = mySide() === 'red' ? '<b class="red">Red</b>' : '<b class="blue">Blue</b>';
  if (NET.status === 'lost' || NET.status === 'failed') return 'Online — not connected';
  if (!onlineConnected()) return NET.role === 'host' ? 'Online — send the link to your friend' : 'Online — connecting to your friend…';
  if (!NET.mine) return `Place your army, ${you} — your friend cannot see it`;
  if (!NET.theirs.hash) return 'Your army is locked in — waiting for your friend';
  return 'Both armies are locked in — revealing…';
}

function onlineResultText() {
  if (NET.desync) return `⚠ ${NET.desync}`;
  const { final, theirFinal } = NET.sums;
  if (final && theirFinal) return '✓ Your friend\'s page computed exactly the same battle.';
  if (!onlineConnected()) return 'Your friend left before confirming this result.';
  return 'Your friend\'s page will confirm this result when it gets there.';
}

function onlineDecorateResult(frame) {
  const on = S.mode === 'online';
  $('#btnResultEdit').hidden = on;
  $('#btnResultNew').hidden = on;
  $('#btnResultRematch').hidden = !on;
  $('#resultOnline').hidden = !on;
  if (!on) return;
  const banner = $('#resultBanner');
  banner.textContent = frame.winner === 'draw' ? 'Draw' : frame.winner === mySide() ? 'You win!' : 'Your friend wins';
  updateResultOnline();
}

function updateResultOnline() {
  if (S.mode !== 'online') return;
  $('#resultOnline').textContent = onlineResultText();
  const btn = $('#btnResultRematch');
  btn.disabled = !onlineConnected() || NET.rematch === 'requested';
  btn.textContent = !onlineConnected() ? 'Friend left' : NET.rematch === 'requested' ? 'Rematch asked…'
    : NET.rematch === 'offered' ? 'Accept rematch' : 'Rematch (swap sides)';
}

function refresh() {
  if (S.mode !== 'online') return;
  updateSides();
  updateScoreboard();
  updateOnlineUi();
  updateResultOnline();
  S.dirty = true;
}

// Called by updateModeUi() and whenever the connection or the round changes.
function updateOnlineUi() {
  const on = S.mode === 'online';
  $('#onlinePrep').hidden = !on;
  $('#onlineInfo').hidden = !on;
  $('#btnStart').hidden = on;
  $('#btnBackToPrep').hidden = on;
  $('#btnNew').textContent = on ? 'Rematch' : 'New game';
  $('#btnNew').title = on ? 'Ask your friend for a rematch (sides swap)' : 'Start over with empty armies';
  for (const sel of ['#presetSelect', '#countRange', '#countInput']) $(sel).disabled = on && !!NET.mine;
  if (!on) {
    $('#btnNew').disabled = false;
    return;
  }
  const me = mySide();
  const guest = NET.role === 'guest';
  for (const sel of ['#presetSelect', '#btnReroll', '#countRow']) $(sel).hidden = guest;
  $('#presetDesc').hidden = true;  // room for the connection status
  if (NET.mine) $('#btnReroll').disabled = true;
  for (const owner of ['red', 'blue']) {
    $(`.score.${owner} .label`).textContent = `${cap(owner)} · ${owner === me ? 'you' : 'friend'}`;
  }
  const status = $('#onlineStatus');
  const bad = NET.status === 'lost' || NET.status === 'failed';
  status.classList.toggle('bad', bad || !!NET.desync);
  const side = `you play <b class="${me}">${cap(me)}</b>`;
  status.innerHTML = {
    off: '', creating: 'Creating a game…',
    waiting: `Invite your friend (${side}):`,
    connecting: 'Connecting to your friend…',
    connected: NET.desync ? `⚠ ${NET.desync}` : `Connected · ${side}${guest ? ' · the host picks the settings' : ''}`,
  }[NET.status] ?? NET.problem;
  $('#onlineLink').hidden = NET.status !== 'waiting';
  $('#onlineLink').value = NET.link;
  $('#btnOnlineCopy').hidden = NET.status !== 'waiting';
  $('#btnOnlineNew').hidden = !bad;
  $('#btnNew').disabled = !(S.phase === 'battle' && onlineConnected()) && !(S.phase === 'prep' && !NET.mine);
  if (S.phase === 'prep') $('#btnNew').textContent = 'Clear army';
  const info = $('#onlineInfo');
  info.textContent = NET.desync ? `⚠ ${NET.desync}`
    : !onlineConnected() ? `Online · ${cap(me)} · ${bad ? 'friend disconnected' : 'not connected'}`
    : `Online · you are ${cap(me)}${NET.rematch === 'offered' ? ' · your friend wants a rematch' : NET.rematch === 'requested' ? ' · rematch asked' : ''}`;
}

// ---------------------------------------------------------------------------
// Wiring of the online controls
// ---------------------------------------------------------------------------
// Called by wire() in app.js.
function wireOnline() {
  $('#btnOnlineCopy').addEventListener('click', async () => {
    const link = $('#onlineLink');
    try {
      await navigator.clipboard.writeText(link.value);
      toast('Link copied - send it to your friend.');
    } catch {
      link.select();
      toast('Copy the selected link (Ctrl+C).');
    }
  });
  $('#onlineLink').addEventListener('focus', e => e.target.select());
  $('#btnOnlineNew').addEventListener('click', onlineNewLink);
  $('#btnOnlineLeave').addEventListener('click', () => setMode('duo'));
  $('#btnResultRematch').addEventListener('click', requestRematch);
  window.addEventListener('pagehide', () => { if (S.mode === 'online' && onlineConnected()) send({ t: 'bye' }); });
}
