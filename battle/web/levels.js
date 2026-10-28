// Life Duel - the one-player levels: Solo and Challenge.
//
// Both run without a generation limit, like the normal game: a battle ends when
// the board repeats (or an army dies out), and cells inside their own starting
// area never count.
//
// Solo: Red plays alone; there is no Blue. The battlefield holds grey cells
// (and on some levels a grey army on Blue's side). The score is the number of
// red cells outside Red's own area when the board starts repeating (the
// average over one cycle; 0 if Red dies out). Under the Birthright rule grey
// cells never turn red themselves: red spreads by births next to grey, so
// converting the grey and growing red are the same thing, and red cells are
// what can be counted.
//
// Challenge: Red against a fixed, visible bot army (Blue). Beating it counts;
// the record is the fewest living red cells on the starting board that still
// win, with the points spent as the tie-break.
//
// Levels are plain data (LEVELS); records stay in this browser.
//
// Loaded before app.js; uses its globals (S, $, toast, ...) only when called.
'use strict';

const LEVEL_STORE = 'lifeDuel.levels';

// The board of every level so far: that of the shipped bot leagues
// (assets/leagues/*-260x128.txt). Unlike their training, there is no
// generation limit and home cells don't count (see useLevel).
const LEAGUE_RULES = { w: 260, h: 128, budget: 120, variant: 'birth' };

// Armies are written as in a life_arena league file, in Red's frame:
// pattern@x,y,rROT,mMIRROR;...  (see parsePieces).
// The champion of No man's land: army "pool 117" of the pool training
// data/pool/n100m.txt, the best of its 100 armies against the Nash mixture of
// the open league (tools/heldout.py: +0.567).
const CHAMPION = 'acorn@36,6,r0,m1;mwss@46,5,r2,m1;block@41,36,r3,m0;mwss@43,43,r2,m1;acorn@30,57,r0,m1;' +
  'mwss@46,57,r0,m0;cell@21,44,r2,m1;cell@44,113,r1,m1;blinker@48,35,r2,m1;cell@16,36,r3,m0;beehive@14,59,r1,m0;' +
  'lwss@36,86,r2,m1;cell@31,108,r3,m0;acorn@40,109,r2,m0;lwss@45,123,r0,m0;cell@39,55,r3,m1;acorn@22,119,r2,m0;' +
  'cell@26,125,r1,m1;cell@35,124,r1,m0;cell@9,102,r0,m1';

// Each level: a battlefield, its rules, and what it holds in each mode.
//   solo:      { text, grey?, computer? }  grey: an army that stands in grey where Blue's would
//   challenge: { text?, bot, botName, computer? }  bot: Blue's army (mirror image of these pieces)
// computer: { pieces, note, score? }  the computer's best army (life_challenge, data/challenge/):
//   in Solo the highest score, in Challenge the fewest cells that won. It searched from random
//   armies, without the players' records.
// Records are kept per mode and level id: give a level a new id when it changes.
const LEVELS = [
  {
    id: 'hive', name: 'Hive lattice', rules: LEAGUE_RULES,
    field: { preset: 'hive_lattice', count: 900, seed: 1 },
    solo: {
      text: 'Break the grey beehive lattice and turn the field red.',
      computer: {
        score: 1012.5,
        pieces: 'cell@12,2,r0,m0;cell@13,2,r0,m0;cell@11,3,r0,m0;cell@14,3,r0,m0;cell@12,4,r0,m0;cell@13,4,r0,m0;' +
          'cell@10,5,r0,m0;cell@11,6,r0,m0;cell@9,7,r0,m0;cell@10,7,r0,m0;cell@11,7,r0,m0;cell@39,23,r0,m0;' +
          'cell@40,23,r0,m0;cell@39,24,r0,m0;cell@41,24,r0,m0;cell@40,25,r0,m0;cell@28,33,r0,m0;' +
          'cell@29,33,r0,m0;cell@0,34,r0,m0;cell@1,34,r0,m0;cell@7,34,r0,m0;cell@8,34,r0,m0;cell@28,34,r0,m0;' +
          'cell@29,34,r0,m0;cell@0,35,r0,m0;cell@1,35,r0,m0;cell@7,35,r0,m0;cell@8,35,r0,m0;cell@4,37,r0,m0;' +
          'cell@5,37,r0,m0;cell@4,38,r0,m0;cell@5,38,r0,m0;cell@22,43,r0,m0;cell@23,43,r0,m0;cell@25,43,r0,m0;' +
          'cell@26,43,r0,m0;cell@21,44,r0,m0;cell@27,44,r0,m0;cell@21,45,r0,m0;cell@28,45,r0,m0;' +
          'cell@31,45,r0,m0;cell@32,45,r0,m0;cell@21,46,r0,m0;cell@22,46,r0,m0;cell@23,46,r0,m0;' +
          'cell@27,46,r0,m0;cell@32,46,r0,m0;cell@26,47,r0,m0;cell@18,54,r0,m0;cell@19,54,r0,m0;' +
          'cell@18,55,r0,m0;cell@19,56,r0,m0;cell@20,56,r0,m0;cell@21,56,r0,m0;cell@16,57,r0,m0;' +
          'cell@17,57,r0,m0;cell@20,57,r0,m0;cell@21,57,r0,m0;cell@18,58,r0,m0;cell@19,58,r0,m0;' +
          'cell@18,59,r0,m0;cell@19,59,r0,m0;cell@15,60,r0,m0;cell@17,60,r0,m0;cell@20,60,r0,m0;' +
          'cell@22,60,r0,m0;cell@15,61,r0,m0;cell@22,61,r0,m0;cell@15,63,r0,m0;cell@22,63,r0,m0;' +
          'cell@16,64,r0,m0;cell@17,64,r0,m0;cell@20,64,r0,m0;cell@21,64,r0,m0;cell@18,65,r0,m0;' +
          'cell@19,65,r0,m0;cell@20,65,r0,m0;cell@18,67,r0,m0;cell@19,67,r0,m0;cell@18,68,r0,m0;' +
          'cell@19,68,r0,m0;cell@17,69,r0,m0;cell@18,69,r0,m0;cell@17,70,r0,m0;cell@15,71,r0,m0;' +
          'cell@17,71,r0,m0;cell@15,72,r0,m0;cell@16,72,r0,m0;cell@4,93,r0,m0;cell@5,94,r0,m0;cell@9,94,r0,m0;' +
          'cell@10,94,r0,m0;cell@3,95,r0,m0;cell@4,95,r0,m0;cell@7,95,r0,m0;cell@9,95,r0,m0;cell@7,97,r0,m0;' +
          'cell@8,97,r0,m0;cell@7,98,r0,m0;cell@8,98,r0,m0;cell@36,116,r0,m0;cell@37,116,r0,m0;' +
          'cell@36,117,r0,m0;cell@37,118,r0,m0;cell@38,118,r0,m0;cell@39,118,r0,m0;cell@39,119,r0,m0;' +
          'cell@25,120,r0,m0;cell@26,120,r0,m0;cell@17,121,r0,m0;cell@18,121,r0,m0;cell@19,121,r0,m0;' +
          'cell@25,121,r0,m0;cell@26,121,r0,m0;cell@17,122,r0,m0;cell@41,125,r0,m0;cell@36,127,r0,m0;' +
          'cell@37,127,r0,m0;cell@38,127,r0,m0',
        note: 'The board repeats at generation 15,489 (life_challenge --solo, 3 minutes of search).',
      },
    },
    challenge: {
      botName: 'Hive bot #103 (9.7% Nash)',
      // assets/leagues/hive-260x128.txt, army #103: the largest Nash weight.
      bot: 'beehive@6,2,r3,m1;block@38,0,r3,m1;block@2,0,r0,m0;r_pentomino@20,18,r1,m0;cell@6,21,r2,m0;' +
        'r_pentomino@36,41,r3,m1;beehive@5,99,r0,m0;r_pentomino@49,49,r0,m1;beehive@42,77,r0,m1;beehive@1,76,r2,m1;' +
        'blinker@44,90,r1,m1;cell@49,69,r1,m1;beehive@17,69,r2,m0;cell@20,80,r2,m0;gosper_gun@28,88,r3,m0;' +
        'cell@47,107,r2,m0;pulsar@1,112,r2,m0;cell@27,121,r2,m1;glider@39,16,r3,m0;cell@5,54,r2,m0',
      computer: {
        pieces: 'cell@46,65,r0,m0;cell@45,66,r0,m0;cell@46,66,r0,m0;cell@47,66,r0,m0;cell@47,67,r0,m0',
        note: 'Wins 335 to 264 when the board repeats at generation 5,947. No army of 3 or 4 cells beats this bot: ' +
          'the computer played all of them (life_challenge --exhaustive 4).',
      },
    },
  },
  {
    id: 'empty', name: "No man's land", rules: LEAGUE_RULES,
    field: { preset: 'empty', count: 900, seed: 1 },
    solo: {
      text: 'Nothing to convert: grow as many red cells out in the field as you can.',
      computer: {
        score: 950.7,
        pieces: 'cell@9,11,r0,m0;cell@46,11,r0,m0;cell@7,12,r0,m0;cell@4,13,r0,m0;cell@5,13,r0,m0;cell@6,13,r0,m0;' +
          'cell@9,13,r0,m0;cell@10,13,r0,m0;cell@12,21,r0,m0;cell@13,21,r0,m0;cell@26,21,r0,m0;' +
          'cell@27,21,r0,m0;cell@28,21,r0,m0;cell@31,21,r0,m0;cell@32,21,r0,m0;cell@13,22,r0,m0;' +
          'cell@29,22,r0,m0;cell@31,23,r0,m0;cell@50,26,r0,m0;cell@51,27,r0,m0;cell@49,28,r0,m0;' +
          'cell@50,28,r0,m0;cell@51,28,r0,m0;cell@12,31,r0,m0;cell@13,31,r0,m0;cell@14,31,r0,m0;' +
          'cell@11,32,r0,m0;cell@15,32,r0,m0;cell@10,33,r0,m0;cell@16,33,r0,m0;cell@23,33,r0,m0;' +
          'cell@10,34,r0,m0;cell@16,34,r0,m0;cell@13,35,r0,m0;cell@1,36,r0,m0;cell@13,36,r0,m0;cell@0,37,r0,m0;' +
          'cell@2,37,r0,m0;cell@48,38,r0,m0;cell@0,39,r0,m0;cell@17,39,r0,m0;cell@18,39,r0,m0;cell@48,39,r0,m0;' +
          'cell@50,39,r0,m0;cell@17,40,r0,m0;cell@18,40,r0,m0;cell@49,41,r0,m0;cell@48,42,r0,m0;' +
          'cell@48,43,r0,m0;cell@48,44,r0,m0;cell@25,53,r0,m0;cell@24,54,r0,m0;cell@25,54,r0,m0;' +
          'cell@26,55,r0,m0;cell@27,55,r0,m0;cell@28,55,r0,m0;cell@27,57,r0,m0;cell@38,73,r0,m0;' +
          'cell@47,73,r0,m0;cell@36,74,r0,m0;cell@37,74,r0,m0;cell@41,74,r0,m0;cell@42,74,r0,m0;' +
          'cell@37,75,r0,m0;cell@41,75,r0,m0;cell@46,75,r0,m0;cell@47,75,r0,m0;cell@48,75,r0,m0;' +
          'cell@36,76,r0,m0;cell@37,77,r0,m0;cell@37,78,r0,m0;cell@36,81,r0,m0;cell@37,81,r0,m0;' +
          'cell@36,82,r0,m0;cell@46,104,r0,m0;cell@48,104,r0,m0;cell@49,105,r0,m0;cell@49,106,r0,m0;' +
          'cell@21,107,r0,m0;cell@22,107,r0,m0;cell@46,107,r0,m0;cell@49,107,r0,m0;cell@21,108,r0,m0;' +
          'cell@22,108,r0,m0;cell@47,108,r0,m0;cell@48,108,r0,m0;cell@49,108,r0,m0;cell@1,110,r0,m0;' +
          'cell@20,110,r0,m0;cell@21,110,r0,m0;cell@22,110,r0,m0;cell@23,110,r0,m0;cell@0,111,r0,m0;' +
          'cell@1,111,r0,m0;cell@19,111,r0,m0;cell@20,111,r0,m0;cell@23,111,r0,m0;cell@24,111,r0,m0;' +
          'cell@18,112,r0,m0;cell@3,113,r0,m0;cell@4,113,r0,m0;cell@5,113,r0,m0;cell@26,113,r0,m0;' +
          'cell@17,114,r0,m0;cell@25,114,r0,m0;cell@7,115,r0,m0;cell@8,115,r0,m0;cell@9,115,r0,m0;' +
          'cell@10,115,r0,m0;cell@29,116,r0,m0;cell@33,116,r0,m0;cell@28,117,r0,m0;cell@28,118,r0,m0;' +
          'cell@33,118,r0,m0;cell@28,119,r0,m0;cell@29,119,r0,m0;cell@30,119,r0,m0;cell@32,119,r0,m0;' +
          'cell@29,121,r0,m0',
        note: 'The board repeats at generation 8,027 (life_challenge --solo, 3 minutes of search).',
      },
    },
    challenge: {
      botName: 'Open bot #146 (9.6% Nash)',
      // assets/leagues/open-260x128.txt, army #146: the largest Nash weight.
      bot: 'acorn@36,6,r0,m1;mwss@44,8,r2,m1;acorn@30,51,r0,m1;cell@22,56,r1,m0;cell@21,33,r1,m1;acorn@17,15,r1,m0;' +
        'blinker@11,36,r2,m1;hwss@25,65,r2,m1;mwss@46,76,r2,m0;glider@23,95,r1,m0;beehive@18,103,r3,m0;' +
        'cell@21,103,r0,m1;hwss@42,119,r2,m1;acorn@45,110,r2,m0',
      computer: {
        pieces: 'cell@36,118,r0,m0;cell@36,119,r0,m0;cell@36,120,r0,m0',
        note: 'A single blinker wins 431.5 to 219 when the board repeats at generation 4,610. ' +
          'Fewer cells cannot win: 1 or 2 cells die at once.',
      },
    },
  },
  {
    id: 'champion', name: 'Champion', rules: LEAGUE_RULES,
    field: { preset: 'empty', count: 900, seed: 1 },
    solo: {
      text: 'The champion army stands in grey on Blue\'s side and comes at you. Make it red.',
      grey: CHAMPION,
      computer: {
        score: 1121.3,
        pieces: 'cell@12,1,r0,m0;cell@22,7,r0,m0;cell@22,8,r0,m0;cell@22,9,r0,m0;cell@22,10,r0,m0;cell@21,11,r0,m0;' +
          'cell@23,11,r0,m0;cell@36,11,r0,m0;cell@39,11,r0,m0;cell@22,12,r0,m0;cell@40,12,r0,m0;' +
          'cell@22,13,r0,m0;cell@36,13,r0,m0;cell@40,13,r0,m0;cell@37,14,r0,m0;cell@38,14,r0,m0;' +
          'cell@39,14,r0,m0;cell@40,14,r0,m0;cell@7,18,r0,m0;cell@6,19,r0,m0;cell@8,19,r0,m0;cell@6,20,r0,m0;' +
          'cell@8,20,r0,m0;cell@7,21,r0,m0;cell@26,26,r0,m0;cell@47,29,r0,m0;cell@51,30,r0,m0;cell@45,31,r0,m0;' +
          'cell@51,31,r0,m0;cell@16,32,r0,m0;cell@45,32,r0,m0;cell@46,32,r0,m0;cell@51,32,r0,m0;' +
          'cell@14,33,r0,m0;cell@15,33,r0,m0;cell@16,33,r0,m0;cell@37,33,r0,m0;cell@38,33,r0,m0;' +
          'cell@39,33,r0,m0;cell@40,33,r0,m0;cell@15,34,r0,m0;cell@37,34,r0,m0;cell@41,34,r0,m0;' +
          'cell@37,35,r0,m0;cell@38,36,r0,m0;cell@18,37,r0,m0;cell@41,37,r0,m0;cell@15,40,r0,m0;' +
          'cell@40,43,r0,m0;cell@43,43,r0,m0;cell@44,44,r0,m0;cell@31,45,r0,m0;cell@32,45,r0,m0;' +
          'cell@40,45,r0,m0;cell@44,45,r0,m0;cell@31,46,r0,m0;cell@32,46,r0,m0;cell@41,46,r0,m0;' +
          'cell@42,46,r0,m0;cell@43,46,r0,m0;cell@44,46,r0,m0;cell@34,48,r0,m0;cell@35,48,r0,m0;' +
          'cell@34,49,r0,m0;cell@35,49,r0,m0;cell@34,60,r0,m0;cell@35,60,r0,m0;cell@36,60,r0,m0;' +
          'cell@33,61,r0,m0;cell@36,61,r0,m0;cell@36,62,r0,m0;cell@32,63,r0,m0;cell@36,63,r0,m0;' +
          'cell@32,64,r0,m0;cell@36,64,r0,m0;cell@36,65,r0,m0;cell@33,66,r0,m0;cell@35,66,r0,m0;' +
          'cell@6,69,r0,m0;cell@6,70,r0,m0;cell@7,70,r0,m0;cell@8,70,r0,m0;cell@14,70,r0,m0;cell@11,71,r0,m0;' +
          'cell@8,72,r0,m0;cell@9,72,r0,m0;cell@37,82,r0,m0;cell@38,82,r0,m0;cell@37,83,r0,m0;cell@38,83,r0,m0;' +
          'cell@39,83,r0,m0;cell@39,84,r0,m0;cell@39,85,r0,m0;cell@40,85,r0,m0;cell@26,93,r0,m0;' +
          'cell@26,94,r0,m0;cell@27,94,r0,m0;cell@36,99,r0,m0;cell@39,99,r0,m0;cell@6,100,r0,m0;' +
          'cell@40,100,r0,m0;cell@6,101,r0,m0;cell@36,101,r0,m0;cell@40,101,r0,m0;cell@6,102,r0,m0;' +
          'cell@37,102,r0,m0;cell@38,102,r0,m0;cell@39,102,r0,m0;cell@40,102,r0,m0;cell@22,119,r0,m0;' +
          'cell@23,119,r0,m0;cell@24,119,r0,m0;cell@25,121,r0,m0;cell@44,123,r0,m0;cell@45,123,r0,m0;' +
          'cell@43,124,r0,m0;cell@46,124,r0,m0;cell@44,125,r0,m0;cell@45,125,r0,m0',
        note: 'The board repeats at generation 4,327 (life_challenge --solo, 3 minutes of search).',
      },
    },
    challenge: {
      text: 'The champion is the best army of our pool training on No man\'s land.',
      botName: 'the champion (+0.57 vs the open bot)',
      bot: CHAMPION,
      computer: {
        pieces: 'cell@38,85,r0,m0;cell@38,86,r0,m0;cell@38,87,r0,m0',
        note: 'A single blinker wins 355 to 263.3 when the board repeats at generation 3,228. ' +
          'Fewer cells cannot win: 1 or 2 cells die at once.',
      },
    },
  },
];

const isLevelMode = (mode = S.mode) => mode === 'solo' || mode === 'challenge';
const levelsFor = mode => LEVELS.filter(l => l[mode]);

function currentLevel() {
  const list = levelsFor(S.mode);
  return list.find(l => l.id === S.level.id) || list[0];
}

// 'acorn@36,6,r0,m1;...' -> [{ pattern, x, y, rot, mirror }]
function parsePieces(text) {
  return text.split(';').filter(Boolean).map(t => {
    const [pattern, at] = t.split('@');
    const [x, y, rot, mirror] = at.split(',');
    return { pattern, x: +x, y: +y, rot: +rot.slice(1) & 3, mirror: mirror === 'm1' };
  });
}

// ---------------------------------------------------------------------------
// Records (this browser only)
// ---------------------------------------------------------------------------
function readRecords() {
  try {
    const all = JSON.parse(localStorage.getItem(LEVEL_STORE) || '{}');
    return all && typeof all === 'object' ? all : {};
  } catch { return {}; }
}

function levelRecord(mode, level) {
  const r = readRecords()[`${mode}/${level.id}`];
  return r && typeof r === 'object' ? r : null;
}

// Solo: a higher score wins. Challenge: fewer cells, then fewer points.
function isBetter(mode, run, best) {
  if (!best) return true;
  if (mode === 'solo') return run.score > best.score;
  return run.cells < best.cells || (run.cells === best.cells && run.cost < best.cost);
}

function storeRecord(mode, level, run) {
  const all = readRecords();
  all[`${mode}/${level.id}`] = { ...run, when: new Date().toISOString().slice(0, 10) };
  try {
    localStorage.setItem(LEVEL_STORE, JSON.stringify(all));
  } catch { /* storage unavailable: the record lasts until the page is closed */ }
}

const soloScore = n => (Math.round(n * 10) / 10).toLocaleString('en-US');

function recordText(mode, record) {
  if (!record) return mode === 'solo' ? 'no score yet' : 'not beaten yet';
  return mode === 'solo' ? `best ${soloScore(record.score)}` : `best ${fmt(record.cells)} cells · ${record.cost} pts`;
}

// ---------------------------------------------------------------------------
// Mode and level switching
// ---------------------------------------------------------------------------
// Switches to Solo or Challenge with level `id` (the first one if unknown).
async function useLevel(id, mode) {
  if (S.mode === 'duo') S.duo = { settings: { ...S.settings }, field: { ...S.field } };
  S.mode = mode;
  S.bot.army = null;
  S.level.id = id;
  const level = currentLevel();
  S.level.id = level.id;
  const r = level.rules;
  const size = S.config.sizes.find(z => z.width === r.w && z.height === r.h);
  // Everything is fixed so that records are comparable: the battle always runs
  // to its end, and home cells never count (in Solo, staying at home is no conversion).
  Object.assign(S.settings, {
    sizeId: size ? size.id : 'level', w: r.w, h: r.h, budget: r.budget, unlimited: true,
    ignoreHome: true, variant: r.variant, blind: false,
  });
  S.field = { ...level.field };
  await rebuildBoard();
}


// resetPreparation(): only Red places; Blue is the bot's fixed army or absent.
function levelFill() {
  S.ready = { red: false, blue: true };
  S.turn = 'red';
  S.open = 'red';
  S.placements = S.placements.filter(p => p.owner === 'red');
  const level = currentLevel();
  if (S.mode === 'challenge' && level.challenge.bot) {
    const bot = botPlacements(fromRedFrame(parsePieces(level.challenge.bot), 'blue', S.settings.w), 'blue');
    S.placements.push(...bot.map(p => ({ ...p, id: S.nextId++ })));
  }
  updateLevelUi();
}

// loadBattlefield(): a Solo level's grey army, on Blue's side as Blue's mirror image would stand.
function levelGrey(grey) {
  if (S.mode !== 'solo') return grey;
  const level = currentLevel();
  if (!level.solo.grey) return grey;
  const { w } = S.settings;
  for (const p of botPlacements(fromRedFrame(parsePieces(level.solo.grey), 'blue', w), 'blue')) {
    for (const [cx, cy] of p.cells) grey[(p.y + cy) * w + p.x + cx] = GREY;
  }
  return grey;
}

// What Red has placed, for the record: living cells, points and pieces.
function levelRun() {
  let cells = 0;
  for (const v of prepBoard()) cells += v === RED;
  const army = sideArmy('red');
  return { cells, cost: spent('red'), pieces: army ? army.pieces : [] };
}

// Replaces Red's placements by `pieces` (Red's frame).
function loadLevelArmy(pieces, what) {
  if (S.phase !== 'prep') return;
  if (armyShapeProblem(pieces, zones().zw, S.settings.h)) return toast(`${what} does not fit this board.`);
  if (S.drag) endDrag();
  S.placements = S.placements.filter(p => p.owner !== 'red')
    .concat(botPlacements(pieces, 'red').map(p => ({ ...p, id: S.nextId++ })));
  S.ready.red = false;
  S.open = 'red';
  boardChanged();
  toast(`Loaded ${what}.`);
}

function loadBestArmy() {
  const record = levelRecord(S.mode, currentLevel());
  if (!record || !Array.isArray(record.pieces)) return;
  loadLevelArmy(record.pieces, 'the army of your record');
}

// The computer's best army of a level in `mode`: { pieces, cells, score, note } or null.
function computerArmy(level, mode = S.mode) {
  const c = level[mode] && level[mode].computer;
  if (!c) return null;
  const pieces = parsePieces(c.pieces);
  let cells = 0;
  for (const p of pieces) {
    const pattern = S.arsenal.get(p.pattern);
    if (pattern) cells += pattern.cells.length;
  }
  return { pieces, cells, score: c.score, note: c.note };
}

// Same pieces, in any order.
const piecesKey = pieces => pieces.map(p => `${p.pattern}@${p.x},${p.y},${p.rot & 3},${p.mirror ? 1 : 0}`).sort().join(';');

function loadComputerArmy() {
  const c = computerArmy(currentLevel());
  if (c) loadLevelArmy(c.pieces, S.mode === 'solo' ? `the computer's army (score ${soloScore(c.score)})` : `the computer's army (${fmt(c.cells)} cells)`);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
function updateLevelUi() {
  if (!isLevelMode()) return;
  const mode = S.mode, level = currentLevel();
  const select = $('#levelSelect');
  select.innerHTML = '';
  for (const l of levelsFor(mode)) select.append(new Option(`${l.name} · ${recordText(mode, levelRecord(mode, l))}`, l.id));
  select.value = level.id;
  const record = levelRecord(mode, level);
  $('#btnLevelBest').disabled = !(record && Array.isArray(record.pieces) && record.pieces.length) || S.phase !== 'prep';
  $('#btnLevelBest').title = record ? `Put the army of your record on the board (${recordText(mode, record)}${record.when ? `, ${record.when}` : ''})`
    : 'Your record army will be kept here';
  const computer = computerArmy(level, mode);
  $('#btnLevelComputer').hidden = !computer;
  $('#btnLevelComputer').disabled = S.phase !== 'prep';
  const computerText = !computer ? '' : mode === 'solo' ? `The computer scored ${soloScore(computer.score)}.`
    : `The computer won with ${fmt(computer.cells)} cells.`;
  if (computer) $('#btnLevelComputer').title = `Put the computer's best army on the board. ${computerText} ${computer.note}`;
  let rules = mode === 'solo'
    ? 'Score: red cells outside your area when the board repeats (no generation limit).'
    : `Beat ${level.challenge.botName} with as few red cells as you can (no generation limit, home cells don't count).`;
  if (computer) rules += ` ${computerText}`;
  $('#levelDesc').textContent = level[mode].text ? `${level[mode].text} ${rules}` : rules;
  $('#levelDesc').title = $('#levelDesc').textContent;
}

function levelPhaseLabel() {
  if (S.mode === 'solo') {
    return S.ready.red ? 'Your army is ready — start the battle!'
      : 'Solo: place your <b class="red">Red</b> army and turn as much of the field red as you can';
  }
  return S.ready.red ? 'Your army is ready — start the battle!'
    : 'Challenge: beat the <b class="blue">bot</b> with as few <b class="red">red</b> cells as you can';
}

// The result dialog of a level battle: banner, reason and the record line.
function levelResult(frame) {
  const b = S.battle, level = LEVELS.find(l => l.id === b.level.id);
  const mode = b.level.mode;
  const best = levelRecord(mode, level);
  let line;
  if (mode === 'solo') {
    const run = { ...b.level.run, score: frame.redScore };
    const computer = computerArmy(level, mode);
    // The computer's army itself is not a record of yours.
    const borrowed = !!computer && piecesKey(run.pieces) === piecesKey(computer.pieces);
    const better = !borrowed && isBetter(mode, run, best);
    if (better && run.score > 0) storeRecord(mode, level, run);
    $('#resultBanner').className = 'result-banner red';
    $('#resultBanner').textContent = `Score ${soloScore(run.score)}`;
    const grey = frame.history ? frame.history.grey : null;
    const greyText = grey && grey[0] ? ` Grey cells: ${fmt(grey[0])} at the start, ${fmt(frame.grey)} at the end.` : '';
    let reason;
    if (frame.reason === 'elimination') reason = `Red died out at generation ${fmt(frame.gen)}.`;
    else if (frame.reason === 'repetition') {
      reason = frame.period === 1 ? `The board froze at generation ${fmt(frame.gen)}: ${soloScore(run.score)} red cells outside Red's area.`
        : `The board started repeating every ${frame.period} generations (at ${fmt(frame.gen)}): ${soloScore(run.score)} red cells outside Red's area on average.`;
    } else reason = `Generation ${fmt(frame.gen)}: ${soloScore(run.score)} red cells outside Red's area.`;
    $('#resultReason').textContent = reason + greyText;
    line = borrowed ? 'The computer\'s army: it does not count as your record.'
      : better && run.score > 0 ? `New record on ${level.name}!${best ? ` (before: ${soloScore(best.score)})` : ''}`
        : `${level.name}: your record is ${best ? soloScore(best.score) : '0'}.`;
    line += ` Your army: ${fmt(run.cells)} cells, ${run.cost} pts.`;
    if (computer && !borrowed) line += ` The computer's best: ${soloScore(computer.score)}.`;
  } else {
    const run = b.level.run;
    const won = frame.winner === 'red';
    const computer = computerArmy(level, mode);
    // The computer's army itself is not a record of yours.
    const borrowed = !!computer && piecesKey(run.pieces) === piecesKey(computer.pieces);
    const better = won && !borrowed && isBetter(mode, run, best);
    if (better) storeRecord(mode, level, run);
    const used = `${fmt(run.cells)} cells (${run.cost} pts)`;
    if (borrowed) line = `The computer's army ${won ? 'won' : 'did not win'} with ${used}. It does not count as your record.`;
    else if (better) line = `Won with ${used} — new record on ${level.name}!${best ? ` Before: ${fmt(best.cells)} cells (${best.cost} pts).` : ''}`;
    else if (won) line = `Won with ${used}. Your record: ${fmt(best.cells)} cells (${best.cost} pts).`;
    else line = `You used ${used}. ${best ? `Your record: ${fmt(best.cells)} cells (${best.cost} pts).` : 'Not beaten yet: beat the bot to set a record.'}`;
    line += ` The bot: ${level.challenge.botName}.`;
    if (computer && !borrowed) line += ` The computer's best: ${fmt(computer.cells)} cells.`;
  }
  $('#resultLevel').hidden = false;
  $('#resultLevel').textContent = line;
  updateLevelUi();
}

function wireLevels() {
  $('#levelSelect').addEventListener('change', e => useLevel(e.target.value, S.mode));
  $('#btnLevelBest').addEventListener('click', loadBestArmy);
  $('#btnLevelComputer').addEventListener('click', loadComputerArmy);
}
