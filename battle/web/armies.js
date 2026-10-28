// Life Duel - saved armies: named, kept in this browser, and published for
// every player through GitHub issues.
//
// An army is stored as Red would place it: its pieces relative to the own zone
// at the left edge, facing right. Blue gets the mirror image (x' = W - x - w,
// mirrored), the same convention as the bot leagues. An army fits every board
// whose zone has the same width and height.
//
// Every army has an ID: the first 12 hex digits of the SHA-256 of its living
// cells in the zone. Equal boards get equal IDs, whoever built them and with
// whichever pieces; saved, published and bot armies share the same IDs.
//
// Army code, for publishing and for passing on by hand (one line):
//   LD1|<zone width>x<height>|<name, URI-encoded>|pattern.x.y.rot.mirror,...
//
// Publishing opens a pre-filled issue in ARMY_REPO; the player only clicks
// "Submit". Open issues that carry a valid army code are the published armies,
// read with the public GitHub API (no login needed to browse). Closing an issue
// unpublishes it. The same army published twice keeps its first issue.
//
// Loaded before app.js; uses its globals (S, $, toast, ...) only when called.
'use strict';

const ARMY_REPO = window.LIFE_DUEL_ARMY_REPO || 'Ascanius3791/Life_Duel';
const ARMY_STORE = 'lifeDuel.armies';
const ARMY_CODE = /LD1\|(\d+)x(\d+)\|([^|\s]*)\|([\w.,-]*)/;
const ARMY_NAME_MAX = 40;
const PUBLISHED_CACHE = 'lifeDuel.published';
const PUBLISHED_TTL_MS = 3 * 60 * 1000;
const PUBLISHED_PAGES = 5;  // up to 500 open issues

const ARMY = {
  dialogOwner: null,      // the side "Load" puts an army into, or null: any side that is being placed
  tab: 'mine',            // 'mine' | 'published'
  published: null,        // [{ id, name, zw, h, cost, pieces, author, issue, url, created }] or null: not loaded
  publishedError: '',
  loadingPublished: null, // the pending fetch
  idCache: new Map(),
};

// ---------------------------------------------------------------------------
// Army model
// ---------------------------------------------------------------------------
const cleanName = name => String(name || '').replace(/[\u0000-\u001f\u007f]/g, ' ').trim().slice(0, ARMY_NAME_MAX);

function armyCost(pieces) {
  return pieces.reduce((sum, p) => sum + (S.arsenal.get(p.pattern) || { cost: 0 }).cost, 0);
}

// Why `pieces` (Red's frame) cannot be an army in a zone of zw x h, or null.
function armyShapeProblem(pieces, zw, h) {
  if (!Array.isArray(pieces) || !pieces.length) return 'it has no pieces';
  for (const p of pieces) {
    const pattern = S.arsenal.get(p.pattern);
    if (!pattern) return `unknown pattern ${p.pattern}`;
    if (![p.x, p.y, p.rot].every(Number.isInteger)) return `bad placement of ${p.pattern}`;
    const shape = oriented(pattern, { rot: p.rot & 3, mirror: !!p.mirror });
    if (p.x < 0 || p.y < 0 || p.x + shape.w > zw || p.y + shape.h > h) return `${pattern.name} at ${p.x},${p.y} is outside the zone`;
  }
  return null;
}

function armyCells(pieces, zw) {
  const cells = new Set();
  for (const p of pieces) {
    const shape = oriented(S.arsenal.get(p.pattern), { rot: p.rot & 3, mirror: !!p.mirror });
    for (const [cx, cy] of shape.cells) cells.add((p.y + cy) * zw + p.x + cx);
  }
  return [...cells].sort((a, b) => a - b);
}

function armyId(pieces, zw, h) {
  const key = `${zw}x${h}|${pieces.map(p => `${p.pattern}.${p.x}.${p.y}.${p.rot & 3}.${p.mirror ? 1 : 0}`).join(',')}`;
  let id = ARMY.idCache.get(key);
  if (!id) {
    id = sha256(`life-duel-army/1/${zw}x${h}/${armyCells(pieces, zw).join(',')}`).slice(0, 12);
    ARMY.idCache.set(key, id);
  }
  return id;
}

const normPiece = p => ({ pattern: String(p.pattern), x: p.x, y: p.y, rot: p.rot & 3, mirror: !!p.mirror });

// A placement list of `owner` on a board of width w, in Red's frame.
function toRedFrame(list, owner, w) {
  return list.map(p => {
    const q = normPiece(p);
    if (owner === 'red') return q;
    const shape = oriented(S.arsenal.get(q.pattern), q);
    return { ...q, x: w - q.x - shape.w, mirror: !q.mirror };
  });
}

// Red's frame -> pieces of `owner` on a board of width w.
function fromRedFrame(pieces, owner, w) {
  return owner === 'red' ? pieces.map(normPiece) : toRedFrame(pieces, 'blue', w);
}

function makeArmy(name, pieces, zw, h, extra = {}) {
  const list = pieces.map(normPiece);
  return { id: armyId(list, zw, h), name: cleanName(name) || 'Unnamed army', zw, h, cost: armyCost(list), pieces: list, ...extra };
}

// What `owner` has placed so far, as an army (null if nothing).
function sideArmy(owner, name = '') {
  const list = S.placements.filter(p => p.owner === owner).flatMap(placementPieces);
  if (!list.length) return null;
  return makeArmy(name, toRedFrame(list, owner, S.settings.w), zones().zw, S.settings.h);
}

// Why `army` cannot be played on the current board, or null.
function armyFitProblem(army) {
  const { zw } = zones();
  if (army.zw !== zw || army.h !== S.settings.h) return `it was built for a ${army.zw} x ${army.h} zone, this board has ${zw} x ${S.settings.h}`;
  if (army.cost > S.settings.budget) return `it costs ${army.cost} points, the budget is ${S.settings.budget}`;
  return armyShapeProblem(army.pieces, army.zw, army.h);
}

// Page placements for `army` on `owner`'s side (as botPlacements()).
function armyPlacements(army, owner) {
  return botPlacements(fromRedFrame(army.pieces, owner, S.settings.w), owner);
}

function armyCode(army) {
  const pieces = army.pieces.map(p => `${p.pattern}.${p.x}.${p.y}.${p.rot & 3}.${p.mirror ? 1 : 0}`).join(',');
  return `LD1|${army.zw}x${army.h}|${encodeURIComponent(army.name)}|${pieces}`;
}

// The army in `text` (anywhere in it), or null. Throws with a reason if it is broken.
function parseArmyCode(text) {
  const m = ARMY_CODE.exec(String(text || ''));
  if (!m) return null;
  const zw = +m[1], h = +m[2];
  let name = '';
  try { name = decodeURIComponent(m[3]); } catch { name = m[3]; }
  const pieces = m[4].split(',').filter(Boolean).map(t => {
    const [pattern, x, y, rot, mirror] = t.split('.');
    return { pattern, x: +x, y: +y, rot: +rot, mirror: mirror === '1' };
  });
  if (!(zw > 0 && zw <= 1000 && h > 0 && h <= 1000)) throw new Error('bad zone size');
  const why = armyShapeProblem(pieces, zw, h);
  if (why) throw new Error(why);
  return makeArmy(name, pieces, zw, h);
}

// ---------------------------------------------------------------------------
// This browser's armies
// ---------------------------------------------------------------------------
function myArmies() {
  try {
    const list = JSON.parse(localStorage.getItem(ARMY_STORE) || '[]');
    // Re-derive IDs and costs: the stored values are only a cache.
    return list.filter(a => a && Array.isArray(a.pieces) && !armyShapeProblem(a.pieces, a.zw, a.h))
      .map(a => makeArmy(a.name, a.pieces, a.zw, a.h, { saved: a.saved || 0 }));
  } catch {
    return [];
  }
}

function storeArmies(list) {
  try {
    localStorage.setItem(ARMY_STORE, JSON.stringify(list.map(({ name, zw, h, pieces, saved }) => ({ name, zw, h, pieces, saved }))));
    return true;
  } catch {
    toast('This browser does not allow saving (private window or storage blocked).');
    return false;
  }
}

// Saves (or renames) an army; an army with the same ID is replaced.
function saveArmy(army) {
  const list = myArmies().filter(a => a.id !== army.id);
  list.unshift({ ...army, saved: Date.now() });
  return storeArmies(list);
}

function deleteArmy(id) {
  storeArmies(myArmies().filter(a => a.id !== id));
}

// ---------------------------------------------------------------------------
// Published armies (GitHub issues)
// ---------------------------------------------------------------------------
function publishUrl(army) {
  const body = [
    `A Life Duel army: **${army.name}** · ${army.cost} points · zone ${army.zw} x ${army.h} · ID \`${army.id}\``,
    '',
    'Submit this issue to publish the army: it then shows up for everyone under *Armies → Published* in the game.',
    'Closing the issue unpublishes it. Please leave the code below unchanged.',
    '',
    '```',
    armyCode(army),
    '```',
  ].join('\n');
  const q = new URLSearchParams({ title: `Army: ${army.name} [${army.id}]`, body });
  return `https://github.com/${ARMY_REPO}/issues/new?${q}`;
}

function publishArmy(army) {
  const win = window.open(publishUrl(army), '_blank');
  if (!win) return toast('Allow pop-ups for this page to publish, or try again.');
  win.opener = null;
  toast('Click "Create" on GitHub to publish. Then refresh the Published list.');
}

function readPublishedCache() {
  try {
    const c = JSON.parse(localStorage.getItem(PUBLISHED_CACHE) || 'null');
    return c && c.repo === ARMY_REPO && Date.now() - c.time < PUBLISHED_TTL_MS ? c.issues : null;
  } catch { return null; }
}

async function fetchIssues() {
  const issues = [];
  for (let page = 1; page <= PUBLISHED_PAGES; page++) {
    const res = await fetch(`https://api.github.com/repos/${ARMY_REPO}/issues?state=open&per_page=100&page=${page}&sort=created&direction=asc`,
      { headers: { Accept: 'application/vnd.github+json' } });
    if (res.status === 403 || res.status === 429) throw new Error('GitHub allows 60 requests per hour without login; try again later');
    if (res.status === 404) throw new Error(`github.com/${ARMY_REPO} is not public, so its armies cannot be read`);
    if (!res.ok) throw new Error(`GitHub answered ${res.status}`);
    const batch = await res.json();
    for (const i of batch) {
      if (i.pull_request || !ARMY_CODE.test(i.body || '')) continue;
      issues.push({ number: i.number, url: i.html_url, author: i.user ? i.user.login : '', created: i.created_at, body: i.body });
    }
    if (batch.length < 100) break;
  }
  try { localStorage.setItem(PUBLISHED_CACHE, JSON.stringify({ repo: ARMY_REPO, time: Date.now(), issues })); } catch { /* ignore */ }
  return issues;
}

// Loads the published armies (cached for a few minutes unless `force`).
function loadPublished(force = false) {
  if (ARMY.loadingPublished) return ARMY.loadingPublished;
  const cached = force ? null : readPublishedCache();
  ARMY.loadingPublished = (async () => {
    try {
      const issues = cached || await fetchIssues();
      const byId = new Map();
      for (const i of issues) {
        let army = null;
        try { army = parseArmyCode(i.body); } catch (err) { console.warn(`issue #${i.number}: ${err.message}`); }
        if (!army || byId.has(army.id)) continue;  // the first issue of an army wins
        byId.set(army.id, { ...army, author: i.author, issue: i.number, url: i.url, created: i.created });
      }
      ARMY.published = [...byId.values()].reverse();  // newest first
      ARMY.publishedError = '';
    } catch (err) {
      ARMY.publishedError = err.message;
      if (!ARMY.published) ARMY.published = [];
    } finally {
      ARMY.loadingPublished = null;
    }
    renderArmyList();
    if (typeof onArmiesChanged === 'function') onArmiesChanged();
  })();
  return ARMY.loadingPublished;
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------
// "Save": asks for a name and stores what `owner` has placed.
function openSaveDialog(owner) {
  const army = sideArmy(owner);
  if (!army) return toast('Place some patterns first - there is nothing to save yet.');
  const known = myArmies().find(a => a.id === army.id);
  const dialog = $('#saveArmyDialog');
  dialog.dataset.owner = owner;
  $('#saveArmyName').value = known ? known.name : '';
  $('#saveArmyName').placeholder = `Army ${myArmies().length + 1}`;
  $('#saveArmyInfo').textContent = `ID ${army.id} · ${army.cost} points · ${army.pieces.length} pieces` +
    (known ? ` · already saved as "${known.name}"` : '');
  dialog.showModal();
  $('#saveArmyName').select();
}

function confirmSave(publish) {
  const dialog = $('#saveArmyDialog');
  const input = $('#saveArmyName');
  const army = sideArmy(dialog.dataset.owner, cleanName(input.value) || input.placeholder);
  dialog.close();
  if (!army || !saveArmy(army)) return;
  toast(`Saved "${army.name}" (${army.id}).`);
  if (publish) publishArmy(army);
  if (typeof onArmiesChanged === 'function') onArmiesChanged();
}

// "Load" (owner given) or "Armies" (owner null): the list of saved and published armies.
function openArmiesDialog(owner = null, tab = ARMY.tab) {
  ARMY.dialogOwner = owner;
  ARMY.tab = tab;
  $('#importCode').value = '';
  renderArmyList();
  $('#armiesDialog').showModal();
  if (tab === 'published' && !ARMY.published) loadPublished();
}

// The sides an army could be loaded into right now.
function loadTargets() {
  if (ARMY.dialogOwner) return isActive(ARMY.dialogOwner) ? [ARMY.dialogOwner] : [];
  return ['red', 'blue'].filter(isActive);
}

function loadArmy(army, owner) {
  const why = armyFitProblem(army);
  if (why) return toast(`"${army.name}" does not fit: ${why}.`);
  if (S.drag) endDrag();
  S.placements = S.placements.filter(p => p.owner !== owner)
    .concat(armyPlacements(army, owner).map(p => ({ ...p, id: S.nextId++ })));
  boardChanged();
  $('#armiesDialog').close();
  toast(`${cap(owner)} now has "${army.name}".`);
}

function importArmyCode() {
  const text = $('#importCode').value;
  let army = null;
  try {
    army = parseArmyCode(text);
  } catch (err) {
    return toast(`That army code is broken: ${err.message}.`);
  }
  if (!army) return toast('Paste an army code (it starts with LD1|).');
  if (!saveArmy(army)) return;
  $('#importCode').value = '';
  ARMY.tab = 'mine';
  renderArmyList();
  toast(`Imported "${army.name}" (${army.id}).`);
  if (typeof onArmiesChanged === 'function') onArmiesChanged();
}

async function copyText(text, what) {
  try {
    await navigator.clipboard.writeText(text);
    toast(`${what} copied.`);
  } catch {
    prompt(`Copy the ${what.toLowerCase()}:`, text);
  }
}

// A thumbnail of the army in its zone (Red's view).
function armyThumb(army) {
  const c = document.createElement('canvas');
  c.className = 'army-thumb';
  const k = 2;  // canvas pixels per cell; CSS scales the canvas down a little
  c.width = army.zw * k;
  c.height = army.h * k;
  const g = c.getContext('2d');
  g.fillStyle = '#0c1018';
  g.fillRect(0, 0, c.width, c.height);
  g.fillStyle = COLOR.red;
  for (const i of armyCells(army.pieces, army.zw)) g.fillRect((i % army.zw) * k, Math.floor(i / army.zw) * k, k, k);
  return c;
}

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function button(label, title, onClick, cls = 'ghost small') {
  const b = el('button', cls, label);
  b.type = 'button';
  if (title) b.title = title;
  b.addEventListener('click', onClick);
  return b;
}

function renderArmyList() {
  const dialog = $('#armiesDialog');
  if (!dialog) return;
  for (const b of $$('#armyTabs button')) b.classList.toggle('on', b.dataset.tab === ARMY.tab);
  const targets = loadTargets();
  $('#armiesTitle').textContent = ARMY.dialogOwner ? `Load an army for ${cap(ARMY.dialogOwner)}` : 'Armies';
  $('#armiesHint').textContent = ARMY.tab === 'mine'
    ? 'Saved in this browser only. Publish an army to show it to every player.'
    : `Armies published by players as issues of github.com/${ARMY_REPO}.`;
  $('#btnPublishedRefresh').hidden = ARMY.tab !== 'published';
  $('#importRow').hidden = ARMY.tab !== 'mine';
  const list = $('#armyList');
  list.innerHTML = '';

  let armies;
  if (ARMY.tab === 'mine') {
    armies = myArmies();
    if (!armies.length) list.append(el('p', 'muted army-empty', 'No saved armies yet. Place an army and press Save in its arsenal.'));
  } else {
    if (ARMY.loadingPublished && !ARMY.published) list.append(el('p', 'muted army-empty', 'Loading published armies…'));
    if (ARMY.publishedError) list.append(el('p', 'muted army-empty bad', `Could not load the published armies: ${ARMY.publishedError}.`));
    armies = ARMY.published || [];
    if (ARMY.published && !armies.length && !ARMY.publishedError) list.append(el('p', 'muted army-empty', 'Nobody has published an army yet - be the first!'));
  }
  const publishedIds = new Set((ARMY.published || []).map(a => a.id));
  const mineIds = new Set(myArmies().map(a => a.id));

  for (const army of armies) {
    const row = el('div', 'army-row');
    row.append(armyThumb(army));
    const info = el('div', 'army-info');
    const title = el('div', 'army-name', army.name);
    if (ARMY.tab === 'mine' && publishedIds.has(army.id)) title.append(el('span', 'army-chip', 'published'));
    info.append(title);
    const meta = el('div', 'army-meta');
    meta.append(el('code', 'army-id', army.id), document.createTextNode(` · ${army.cost} pts · ${army.pieces.length} pieces`));
    if (army.author) meta.append(document.createTextNode(` · by ${army.author}`));
    info.append(meta);
    const fit = armyFitProblem(army);
    if (fit) info.append(el('div', 'army-meta bad', `Not for this board: ${fit}.`));
    row.append(info);

    const actions = el('div', 'army-actions');
    for (const owner of targets) {
      const b = button(targets.length > 1 ? `→ ${cap(owner)}` : 'Load', `Put this army on ${cap(owner)}'s side (replaces what is there)`, () => loadArmy(army, owner),
        `small load ${owner}`);
      b.disabled = !!fit;
      actions.append(b);
    }
    if (ARMY.tab === 'mine') {
      actions.append(button('Rename', 'Give this army another name', () => {
        const name = cleanName(prompt('New name:', army.name));
        if (!name) return;
        saveArmy({ ...army, name });
        renderArmyList();
        if (typeof onArmiesChanged === 'function') onArmiesChanged();
      }));
      if (!publishedIds.has(army.id)) actions.append(button('Publish', 'Show this army to every player (opens GitHub)', () => publishArmy(army)));
      actions.append(button('Code', 'Copy the army code, to pass it on by hand', () => copyText(armyCode(army), 'Army code')));
      actions.append(button('✕', 'Delete from this browser', () => {
        if (!confirm(`Delete "${army.name}" from this browser?`)) return;
        deleteArmy(army.id);
        renderArmyList();
        if (typeof onArmiesChanged === 'function') onArmiesChanged();
      }, 'ghost small icon-del'));
    } else {
      const save = button(mineIds.has(army.id) ? 'Saved ✓' : 'Save', 'Keep a copy in this browser', () => {
        if (saveArmy(army)) { toast(`Saved "${army.name}".`); renderArmyList(); }
      });
      save.disabled = mineIds.has(army.id);
      actions.append(save);
      const link = el('a', 'army-link', `#${army.issue}`);
      link.href = army.url;
      link.target = '_blank';
      link.rel = 'noopener';
      link.title = 'The issue on GitHub';
      actions.append(link);
    }
    row.append(actions);
    list.append(row);
  }
}

function wireArmies() {
  for (const owner of ['red', 'blue']) {
    $(`#side${cap(owner)}`).addEventListener('click', e => {
      const btn = e.target.closest('button[data-act]');
      if (btn && btn.dataset.act === 'save') openSaveDialog(owner);
      if (btn && btn.dataset.act === 'load') openArmiesDialog(owner);
    });
  }
  $('#btnArmies').addEventListener('click', () => openArmiesDialog(null));
  for (const b of $$('#armyTabs button')) {
    b.addEventListener('click', () => {
      ARMY.tab = b.dataset.tab;
      renderArmyList();
      if (ARMY.tab === 'published' && !ARMY.published) loadPublished();
    });
  }
  $('#btnPublishedRefresh').addEventListener('click', () => { ARMY.published = null; renderArmyList(); loadPublished(true); });
  $('#btnImport').addEventListener('click', importArmyCode);
  $('#importCode').addEventListener('keydown', e => { if (e.key === 'Enter') importArmyCode(); });
  $('#btnArmiesClose').addEventListener('click', () => $('#armiesDialog').close());
  $('#btnSaveArmy').addEventListener('click', () => confirmSave(false));
  $('#btnSavePublish').addEventListener('click', () => confirmSave(true));
  $('#btnSaveCancel').addEventListener('click', () => $('#saveArmyDialog').close());
  $('#saveArmyName').addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); confirmSave(false); } });
}
