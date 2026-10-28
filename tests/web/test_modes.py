# Browser test of the local modes (2 players, vs Bot, Solo, Challenge) against a running
# Life Duel: the C++ server or the static site. Needs Playwright with Chromium
# (pip install playwright && python3 -m playwright install chromium).
#
#   ./build/life_battle --no-browser --port 18080 &
#   python3 tests/web/test_modes.py http://127.0.0.1:18080/
#   tools/build_site.sh && python3 -m http.server -d dist 18081 &
#   python3 tests/web/test_modes.py http://127.0.0.1:18081/
import sys
from playwright.sync_api import sync_playwright

BASE = sys.argv[1]

PLACE = """(owner) => {
  const pats = [...S.arsenal.values()].sort((a, b) => a.cost - b.cost);
  const [zx0] = zoneRange(owner);
  let i = 0;
  for (const pat of pats) {
    if (i >= 6) break;
    if (pat.cost > remaining(owner)) continue;
    const shape = oriented(pat, S.orient[owner]);
    const x = zx0 + 3 + (i % 3) * 16, y = 6 + Math.floor(i / 3) * 40;
    if (x + shape.w < zoneRange(owner)[1]) { place(owner, pat, shape, x, y); i++; }
  }
  return S.placements.filter(p => p.owner === owner).length;
}"""

def cell_xy(page, x, y):
    """Client coordinates of the centre of board cell (x, y)."""
    return page.evaluate(f"""(() => {{ const r = canvas.getBoundingClientRect();
      return [r.left + (view.ox + ({x} + 0.5) * view.cs) / view.dpr, r.top + (view.oy + ({y} + 0.5) * view.cs) / view.dpr]; }})()""")

def test_placing(page):
    """Advancing a pattern before placing it, and taking single cells out of placed pieces."""
    page.evaluate("newGame()")
    page.wait_for_function("S.grey && S.turn === 'red' && S.open === 'red'")
    page.evaluate("S.grey.fill(0); paintBoard(prepBoard())")  # no grey cells in the way
    red = lambda: page.evaluate("S.placements.filter(p => p.owner === 'red').map(p => ({ id: p.patternId, free: !!p.free, n: p.cells.length, cost: p.cost, x: p.x, y: p.y, cells: p.cells }))")
    left = lambda: page.evaluate("remaining('red')")
    budget = left()

    # A glider one generation on: placed as 5 loose cells. (After 2 it is a
    # mirrored glider, which is placed as the glider piece.)
    page.click('#sideRed .card[data-id=glider]')
    page.mouse.move(*cell_xy(page, 15, 20))
    page.keyboard.press('Period'); page.keyboard.press('Period'); page.keyboard.press('Comma')
    assert page.evaluate("S.drag.gen") == 1
    assert 'generation +1' in page.evaluate("$('#boardHint').textContent")
    page.mouse.click(*cell_xy(page, 15, 20))
    # Four generations on it is a glider again: placed as the glider piece.
    page.keyboard.press('Period'); page.keyboard.press('Period'); page.keyboard.press('Period')
    page.mouse.click(*cell_xy(page, 15, 40))
    pl = red()
    assert [(q['id'], q['free'], q['n']) for q in pl] == [('cell', True, 5), ('glider', False, 5)], pl
    assert left() == budget - 10

    # The Gosper gun 45 generations on (Shift+. = 10 generations).
    page.keyboard.press('Escape')
    page.click('#sideRed .card[data-id=gosper_gun]')
    page.mouse.move(*cell_xy(page, 25, 80))
    for _ in range(4): page.keyboard.press('Shift+Period')
    for _ in range(5): page.keyboard.press('Period')
    want = page.evaluate("dragPattern().cells.length")
    assert page.evaluate("S.drag.gen") == 45 and want != 36
    page.mouse.click(*cell_xy(page, 25, 80))
    page.keyboard.press('Escape')
    gun = red()[-1]
    assert gun['free'] and gun['n'] == want and gun['cost'] == want, gun
    assert left() == budget - 10 - want

    # Ctrl+right-click takes one cell out of the gun, X one out of the glider piece.
    cx, cy = gun['cells'][0]
    page.mouse.move(*cell_xy(page, gun['x'] + cx, gun['y'] + cy))
    assert 'removes just this cell' in page.evaluate("$('#boardHint').textContent")
    page.keyboard.down('Control'); page.mouse.click(*cell_xy(page, gun['x'] + cx, gun['y'] + cy), button='right'); page.keyboard.up('Control')
    assert red()[-1]['n'] == want - 1 and left() == budget - 10 - want + 1
    glider = red()[1]
    gx, gy = glider['cells'][2]
    page.mouse.move(*cell_xy(page, glider['x'] + gx, glider['y'] + gy))
    page.keyboard.press('x')
    pl = red()
    assert (pl[1]['id'], pl[1]['free'], pl[1]['n']) == ('cell', True, 4), pl
    assert left() == budget - 10 - want + 2

    # The army stores loose cells as `cell` pieces: same cells, same cost, legal.
    ok = page.evaluate("""(() => { const a = sideArmy('red'); const board = prepBoard(); const { zw } = zones();
      const cells = []; for (let i = 0; i < board.length; i++) if (board[i] === RED) cells.push(Math.floor(i / S.settings.w) * zw + i % S.settings.w);
      return [armyProblem(a.pieces, 'red', { width: S.settings.w, height: S.settings.h, budget: S.settings.budget }), a.cost === spent('red'),
              JSON.stringify(armyCells(a.pieces, zw)) === JSON.stringify(cells.sort((x, y) => x - y))]; })()""")
    assert ok == [None, True, True], ok

    # A plain right-click still removes the whole (edited) gun.
    gun = red()[-1]
    cx, cy = gun['cells'][0]
    page.mouse.click(*cell_xy(page, gun['x'] + cx, gun['y'] + cy), button='right')
    assert len(red()) == 2 and left() == budget - 9
    print('placing: phases and single cells ok')

def finish(page):
    page.evaluate("skipToEnd()")
    page.wait_for_function("S.battle && S.battle.frame.finished && !S.battle.skipping", timeout=120000)
    page.wait_for_selector('#resultDialog[open]', timeout=10000)
    return page.evaluate("({gen: S.battle.frame.gen, winner: S.battle.frame.winner, reason: S.battle.frame.reason, banner: $('#resultBanner').textContent})")

with sync_playwright() as p:
    browser = p.chromium.launch()
    page = browser.new_page()
    errors = []
    page.on('pageerror', lambda e: errors.append(str(e)))
    page.on('console', lambda m: errors.append(m.text) if m.type == 'error' else None)
    page.goto(BASE)
    page.wait_for_function("S.config && S.grey", timeout=20000)
    page.evaluate("document.querySelectorAll('dialog[open]').forEach(d => d.close())")
    modes = page.evaluate("[...document.querySelectorAll('#modeSwitch button')].filter(b => !b.hidden).map(b => b.dataset.mode)")
    print('modes', modes, 'static backend:', page.evaluate("!!window.lifeDuelBackend"))

    # 2 players (blind placement: red, then blue)
    page.click('#modeSwitch [data-mode=duo]')
    test_placing(page)
    page.evaluate("newGame()")
    page.wait_for_function("S.grey")
    n = page.evaluate(PLACE, 'red'); page.evaluate("setReady('red')")
    m = page.evaluate(PLACE, 'blue'); page.evaluate("setReady('blue')")
    page.click('#btnStart')
    page.wait_for_function("S.battle && S.phase === 'battle'")
    print('2 players:', n, 'vs', m, 'pieces ->', finish(page))
    page.click('#btnResultView')

    # vs Bot
    page.click('#modeSwitch [data-mode=bot]')
    page.wait_for_function("S.mode === 'bot' && S.bot.league && S.grey", timeout=20000)
    h = page.evaluate("humanSide()")
    page.evaluate(PLACE, h); page.evaluate(f"setReady('{h}')")
    page.click('#btnStart')
    page.wait_for_function("S.battle && S.phase === 'battle'")
    print('vs Bot:', finish(page), page.evaluate("$('#resultBot').textContent"))
    page.click('#btnResultView')

    # Solo: Red alone on every level; the score is Red's counted cells.
    page.evaluate("localStorage.removeItem('lifeDuel.levels')")
    page.click('#modeSwitch [data-mode=solo]')
    page.wait_for_function("S.mode === 'solo' && S.grey", timeout=20000)
    for level in page.evaluate("levelsFor('solo').map(l => l.id)"):
        page.select_option('#levelSelect', level)
        page.wait_for_function(f"S.level.id === '{level}' && S.grey && S.phase === 'prep'")
        grey = page.evaluate("S.grey.reduce((n, v) => n + (v === GREY), 0)")
        page.evaluate(PLACE, 'red'); page.evaluate("setReady('red')")
        page.click('#btnStart')
        page.wait_for_function("S.battle && S.phase === 'battle'")
        r = finish(page)
        assert r['reason'] != 'elimination' or r['gen'] > 0, r  # no Blue is no elimination
        assert page.evaluate("S.battle.frame.maxGen === 0 && S.battle.frame.redHome != null")  # no limit, home cells don't count
        score = page.evaluate("S.battle.frame.redScore")
        print(f'Solo {level}: {grey} grey ->', r, 'score', score, '|', page.evaluate("$('#resultLevel').textContent"))
        page.click('#btnResultEdit')
    records = page.evaluate("JSON.parse(localStorage.getItem('lifeDuel.levels') || '{}')")
    print('Solo records:', {k: v['score'] for k, v in records.items()})
    assert all(f'solo/{l}' in records for l in ['hive', 'empty', 'champion'] if records), records

    # Challenge: the fixed bot is visible; a win sets the record (fewest cells).
    page.click('#modeSwitch [data-mode=challenge]')
    page.wait_for_function("S.mode === 'challenge' && S.grey", timeout=20000)
    page.select_option('#levelSelect', 'empty')
    page.wait_for_function("S.level.id === 'empty' && S.grey && S.phase === 'prep'")
    bot = page.evaluate("S.placements.filter(p => p.owner === 'blue').length")
    assert bot > 0 and not page.evaluate("isHidden('blue')")
    page.evaluate(PLACE, 'red'); page.evaluate("setReady('red')")
    page.click('#btnStart')
    page.wait_for_function("S.battle && S.phase === 'battle'")
    print('Challenge empty, weak army vs', bot, 'bot pieces ->', finish(page), '|', page.evaluate("$('#resultLevel').textContent"))
    # The champion (the strongest army of the pool training) as Red against the open bot.
    page.click('#btnResultEdit')
    page.evaluate("S.placements = S.placements.filter(p => p.owner !== 'red').concat(botPlacements(parsePieces(CHAMPION), 'red').map(p => ({ ...p, id: S.nextId++ }))); boardChanged()")
    page.evaluate("setReady('red')")
    page.click('#btnStart')
    page.wait_for_function("S.battle && S.phase === 'battle'")
    r = finish(page)
    assert page.evaluate("S.battle.frame.maxGen === 0 && S.battle.frame.blueHome != null")
    print('Challenge empty, the champion ->', r, '|', page.evaluate("$('#resultLevel').textContent"))
    record = page.evaluate("levelRecord('challenge', currentLevel())")
    assert (record is not None) == (r['winner'] == 'red'), record
    if record:
        page.click('#btnResultEdit')
        page.evaluate("$('[data-act=clear]', $('#sideRed')).click()")
        page.click('#btnLevelBest')
        assert page.evaluate("levelRun().cells") == record['cells']
        print('Challenge record:', record['cells'], 'cells,', record['cost'], 'pts; Best army reloads it')
    print('errors:', errors or 'none')
    browser.close()
