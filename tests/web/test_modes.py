# Browser test of the local modes (2 players, vs Bot) against a running
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
    print('errors:', errors or 'none')
    browser.close()
