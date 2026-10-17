# Browser test of saved armies (save, IDs, load on the other side, import,
# published armies from GitHub issues) and of Watch mode, against a running
# Life Duel (the C++ server or the static site). The GitHub API is mocked.
# Needs Playwright with Chromium (see test_modes.py).
#
#   python3 tests/web/test_armies.py http://127.0.0.1:18080/
import json
import sys
from playwright.sync_api import sync_playwright

BASE = sys.argv[1]

PLACE = """(owner) => {
  const pats = [...S.arsenal.values()].filter(p => p.cost > 1).sort((a, b) => a.cost - b.cost);
  const [zx0, zx1] = zoneRange(owner);
  let i = 0;
  for (const pat of pats) {
    if (i >= 5) break;
    if (pat.cost > remaining(owner)) continue;
    const shape = oriented(pat, S.orient[owner]);
    const x = zx0 + 2 + (i % 2) * 20, y = 5 + i * 22;
    if (x + shape.w < zx1) { place(owner, pat, shape, x, y); i++; }
  }
  return i;
}"""

# The cells of `owner`'s zone, mirrored for Blue, so both sides compare in Red's frame.
ZONE = """(owner) => {
  const b = prepBoard(), { w, h } = S.settings, { zw } = zones(), v = OWNER_CELL[owner], out = [];
  for (let y = 0; y < h; y++) for (let x = 0; x < zw; x++) {
    const bx = owner === 'red' ? x : w - 1 - x;
    if (b[y * w + bx] === v) out.push(y * zw + x);
  }
  return out.join(',');
}"""


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        global failed
        failed = True


failed = False
with sync_playwright() as p:
    browser = p.chromium.launch()
    page = browser.new_page()
    errors = []
    page.on('pageerror', lambda e: errors.append(str(e)))
    page.on('console', lambda m: errors.append(m.text) if m.type == 'error' else None)
    published = []
    page.route('https://api.github.com/**', lambda route: route.fulfill(
        status=200, content_type='application/json', body=json.dumps(published)))
    page.goto(BASE)
    page.wait_for_function("S.config && S.grey", timeout=20000)
    page.evaluate("document.querySelectorAll('dialog[open]').forEach(d => d.close())")
    check(page.evaluate("S.field.preset") == 'hive_lattice', 'Hive lattice is the default battlefield')

    # Save Red's army, load it for Blue: the same cells, mirrored, and the same ID.
    page.click('#modeSwitch [data-mode=duo]')
    page.evaluate("S.settings.blind = false; newGame()")
    n = page.evaluate(PLACE, 'red')
    red_id = page.evaluate("sideArmy('red').id")
    page.evaluate("openDrawer('red')")
    page.click('#sideRed [data-act=save]')
    page.fill('#saveArmyName', 'Test <b>army</b>')
    page.click('#btnSaveArmy')
    saved = page.evaluate("myArmies()")
    check(len(saved) == 1 and saved[0]['id'] == red_id and saved[0]['name'] == 'Test <b>army</b>', f'saved {n} pieces as {red_id}')
    page.evaluate("openDrawer('blue')")
    page.click('#sideBlue [data-act=load]')
    check(page.inner_text('#armyList .army-name') == 'Test <b>army</b>', 'the name is shown as text')
    page.click('#armyList .army-actions button.load')
    check(page.evaluate("sideArmy('blue').id") == red_id, 'loaded for Blue with the same ID')
    check(page.evaluate(ZONE, 'red') == page.evaluate(ZONE, 'blue'), 'Blue got the mirror image of the cells')
    check(page.evaluate("armyProblem(S.placements.filter(p => p.owner === 'blue').map(p => ({pattern: p.patternId, x: p.x, y: p.y, rot: p.rot, mirror: p.mirror})), 'blue', {width: S.settings.w, height: S.settings.h, budget: S.settings.budget})") is None,
          "Blue's army is legal")

    # Army codes: round trip, and a code from another source imports with the same ID.
    code = page.evaluate("armyCode(myArmies()[0])")
    check(page.evaluate("c => parseArmyCode(c).id", code) == red_id, f'army code round trip ({len(code)} chars)')
    page.evaluate("storeArmies([])")
    page.click('#btnArmies')
    page.fill('#importCode', 'look: ' + code + ' (end)')
    page.click('#btnImport')
    check([a['id'] for a in page.evaluate("myArmies()")] == [red_id], 'imported a pasted code')
    page.fill('#importCode', 'LD1|52x128|x|glider.60.0.0.0')
    page.click('#btnImport')
    check(len(page.evaluate("myArmies()")) == 1, 'a code outside the zone is refused')

    # Publish link and published armies.
    url = page.evaluate("publishUrl(myArmies()[0])")
    check(url.startswith('https://github.com/') and '/issues/new?' in url and len(url) < 8000, f'publish link ({len(url)} chars)')
    body = page.evaluate("new URL(publishUrl(myArmies()[0])).searchParams.get('body')")
    other = page.evaluate("armyCode(makeArmy('Glider wall', [{pattern: 'glider', x: 1, y: 1, rot: 0, mirror: false}, {pattern: 'glider', x: 1, y: 9, rot: 0, mirror: false}], 52, 128))")
    published[:] = [
        {'number': 7, 'html_url': 'https://github.com/x/y/issues/7', 'user': {'login': 'alice'}, 'created_at': '2026-10-17T10:00:00Z', 'body': body},
        {'number': 8, 'html_url': 'https://github.com/x/y/issues/8', 'user': {'login': 'bob'}, 'created_at': '2026-10-17T11:00:00Z', 'body': body.replace('Test', 'Copy')},
        {'number': 9, 'html_url': 'https://github.com/x/y/issues/9', 'user': {'login': 'carol'}, 'created_at': '2026-10-17T12:00:00Z', 'body': 'not an army'},
        {'number': 10, 'html_url': 'https://github.com/x/y/issues/10', 'user': {'login': 'dave'}, 'created_at': '2026-10-17T13:00:00Z', 'body': '```\n' + other + '\n```'},
        {'number': 11, 'html_url': 'https://github.com/x/y/pull/11', 'user': {'login': 'eve'}, 'pull_request': {}, 'body': other},
    ]
    page.click('#armyTabs [data-tab=published]')
    page.wait_for_function("ARMY.published && ARMY.published.length")
    pub = page.evaluate("ARMY.published.map(a => [a.name, a.author, a.issue])")
    check(pub == [['Glider wall', 'dave', 10], ['Test <b>army</b>', 'alice', 7]], f'published: first issue per army wins, junk ignored {pub}')
    check(page.locator('#armyList .army-row').count() == 2, 'two published rows')
    page.click('#armyTabs [data-tab=mine]')
    check('published' in page.inner_text('#armyList'), 'my published army is marked')
    page.click('#btnArmiesClose')

    # Watch mode: two league armies, cycling, a saved army on one side.
    page.click('#modeSwitch [data-mode=watch]')
    page.wait_for_function("S.mode === 'watch' && S.bot.league && S.grey", timeout=20000)
    check(page.evaluate("S.bot.id") == 'hive-260x128', 'Watch uses the Hive league by default')
    check(page.evaluate("S.ready.red && S.ready.blue && !isActive('red') && !isActive('blue')"), 'nobody places in Watch mode')
    shown = page.evaluate("[S.watch.shown.red.key, S.watch.shown.blue.key]")
    check(all(k.startswith('bot:') for k in shown), f'random league armies drawn {shown}')
    page.click('#watchPrep .army-pick.red [data-step="1"]')
    first = page.evaluate("S.watch.pick.red")
    page.click('#watchPrep .army-pick.red [data-step="1"]')
    second = page.evaluate("S.watch.pick.red")
    page.click('#watchPrep .army-pick.red [data-step="-1"]')
    check(first != second and page.evaluate("S.watch.pick.red") == first, f'◀ ▶ cycle ({first} -> {second} -> back)')
    page.select_option('#watchPrep .army-pick.blue select', f'mine:{red_id}')
    check(page.evaluate("sideArmy('blue').id") == red_id, 'a saved army can play in Watch mode')
    page.click('#btnStart')
    page.wait_for_function("S.battle && S.phase === 'battle'")
    page.evaluate("skipToEnd()")
    page.wait_for_selector('#resultDialog[open]', timeout=120000)
    print('     watch result:', page.inner_text('#resultBanner'), '|', page.inner_text('#resultBot'))
    page.click('#btnResultNew')
    check(page.evaluate("S.watch.pick.red") == first and page.evaluate("sideArmy('red') !== null"), 'New game keeps a picked army')
    page.reload()
    page.wait_for_function("S.mode === 'watch' && S.bot.league && S.grey", timeout=20000)
    check(page.evaluate("S.watch.pick.red") == first, 'Watch mode and the picks survive a reload')
    check(not errors, f'no page errors {errors}')
    browser.close()
sys.exit(1 if failed else 0)
