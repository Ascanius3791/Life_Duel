# Browser test of online play: host and guest in two separate browser
# contexts on this machine, connected through PeerJS's public signalling server
# (needs internet) and WebRTC. Checks commit-reveal hiding, identical results
# on both pages, rematch with swapped sides, a forged reveal and a disconnect.
#
#   python3 tests/web/test_online.py http://127.0.0.1:18081/             # both on the static site
#   python3 tests/web/test_online.py http://127.0.0.1:18081/ http://127.0.0.1:18080/
#       (host on the static site/WebAssembly, guest on life_battle/native)
import sys
from playwright.sync_api import sync_playwright

BASE = sys.argv[1]
GUEST_BASE = sys.argv[2] if len(sys.argv) > 2 else None  # e.g. guest on the C++ server, host on the static site

PLACE = """([owner, k]) => {
  const pats = [...S.arsenal.values()].sort((a, b) => a.cost - b.cost);
  const [zx0, zx1] = zoneRange(owner);
  S.orient[owner] = { rot: k % 4, mirror: k % 2 === 1 };
  let i = 0;
  for (const pat of pats.slice(k % 3)) {
    if (i >= 5) break;
    if (pat.cost > remaining(owner)) continue;
    const shape = oriented(pat, S.orient[owner]);
    const x = zx0 + 2 + ((i * 7 + k * 3) % 30), y = 4 + ((i * 23 + k * 11) % 90);
    if (x + shape.w <= zx1 && y + shape.h <= S.settings.h) { place(owner, pat, shape, x, y); i++; }
  }
  return S.placements.filter(p => p.owner === owner).length;
}"""

SPY = """() => { window.__got = []; const orig = window.onMessage; window.onMessage = m => { window.__got.push(JSON.parse(JSON.stringify(m))); orig(m); }; }"""

def log(page, name):
    page.on('pageerror', lambda e: print(name, 'PAGEERROR', str(e)[:300]))
    page.on('console', lambda m: print(name, 'console.error', m.text[:200]) if m.type == 'error' else None)

def state(page):
    return page.evaluate("""() => ({ side: mySide(), status: NET.status, round: NET.round, ready: S.ready, phase: S.phase,
      desync: NET.desync, label: $('#phaseLabel').textContent })""")

def finish(page):
    page.wait_for_function("S.phase === 'battle' && S.battle", timeout=30000)
    page.evaluate("skipToEnd()")
    page.wait_for_function("S.battle && S.battle.frame.finished && !S.battle.skipping", timeout=120000)
    page.wait_for_selector('#resultDialog[open]', timeout=10000)
    return page.evaluate("""() => { const f = S.battle.frame; return { gen: f.gen, winner: f.winner, reason: f.reason,
      red: f.red, blue: f.blue, start: NET.sums.start, final: NET.sums.final, banner: $('#resultBanner').textContent }; }""")

with sync_playwright() as p:
    browser = p.chromium.launch()
    host = browser.new_context().new_page(); log(host, 'host')
    guest = browser.new_context().new_page(); log(guest, 'guest')
    host.goto(BASE)
    host.wait_for_function("S.config && S.grey", timeout=30000)
    host.evaluate("document.querySelectorAll('dialog[open]').forEach(d => d.close())")
    host.click('#modeSwitch [data-mode=online]')
    host.wait_for_function("NET.status === 'waiting'", timeout=30000)
    link = host.input_value('#onlineLink')
    print('link:', link)
    host.evaluate(SPY)
    if GUEST_BASE:
        link = GUEST_BASE + link[link.index('#'):]
    guest.goto(link)
    guest.wait_for_function("S.config", timeout=30000)
    guest.evaluate(SPY)
    guest.evaluate("document.querySelectorAll('dialog[open]').forEach(d => d.close())")
    host.wait_for_function("NET.status === 'connected'", timeout=40000)
    guest.wait_for_function("NET.status === 'connected' && NET.round === 1 && S.grey && NET.fieldRound === 1", timeout=40000)
    print('host ', state(host))
    print('guest', state(guest), 'url after join:', guest.url)

    for rnd in range(2):
        hs, gs = host.evaluate("mySide()"), guest.evaluate("mySide()")
        assert hs != gs
        nh = host.evaluate(PLACE, [hs, 1 + rnd]); ng = guest.evaluate(PLACE, [gs, 4 + rnd])
        host.evaluate("setReady(mySide())")
        guest.wait_for_function("S.ready[otherSide(mySide())]", timeout=20000)
        # Commit-reveal: the guest has only a hash of the host's army so far.
        got = guest.evaluate("window.__got.filter(m => (m.t === 'commit' || m.t === 'reveal') && m.round === NET.round)")
        hidden = guest.evaluate("NET.theirs.army === null && S.placements.every(p => p.owner === mySide())")
        print(f'round {rnd + 1}: host {hs} {nh} pieces, guest {gs} {ng} pieces; guest received {[sorted(m.keys()) for m in got]}; host army hidden: {hidden}')
        assert hidden and all('army' not in m for m in got)
        # Budget of the hidden side shows '?'
        print('  guest sees host budget as:', guest.evaluate(f"$('#side{hs.capitalize()} .budget-text').textContent"))
        guest.evaluate("setReady(mySide())")
        rh, rg = finish(host), finish(guest)
        print('  host :', rh)
        print('  guest:', rg)
        host.wait_for_function("NET.sums.theirFinal", timeout=20000)
        guest.wait_for_function("NET.sums.theirFinal", timeout=20000)
        same = {k: rh[k] for k in rh if k != 'banner'} == {k: rg[k] for k in rg if k != 'banner'}
        print('  identical results:', same, '| desync:', host.evaluate('NET.desync') or 'none', guest.evaluate('NET.desync') or 'none',
              '| guest dialog:', guest.evaluate("$('#resultOnline').textContent"))
        assert same
        if rnd == 0:
            host.click('#btnResultRematch')
            guest.wait_for_function("NET.rematch === 'offered'", timeout=20000)
            print('  guest rematch button:', guest.evaluate("$('#btnResultRematch').textContent"))
            guest.click('#btnResultRematch')
            host.wait_for_function("NET.round === 2 && S.phase === 'prep'", timeout=20000)
            guest.wait_for_function("NET.round === 2 && S.phase === 'prep' && NET.fieldRound === 2 && S.grey", timeout=20000)
            print('  after rematch: host', host.evaluate("mySide()"), 'guest', guest.evaluate("mySide()"))

    # A cheating guest: reveals another army than committed -> rejected.
    # (Simulated by feeding a forged reveal to the host's message handler.)
    host.click('#btnResultRematch'); guest.wait_for_function("NET.rematch === 'offered'", timeout=20000); guest.click('#btnResultRematch')
    host.wait_for_function("NET.round === 3 && S.phase === 'prep'", timeout=20000)
    guest.wait_for_function("NET.round === 3 && NET.fieldRound === 3", timeout=20000)
    gs = guest.evaluate("mySide()")
    guest.evaluate(PLACE, [gs, 2])
    guest.evaluate("setReady(mySide())")
    host.wait_for_function("NET.theirs.hash", timeout=20000)
    host.evaluate("""() => { const side = otherSide(mySide()); const [zx0] = zoneRange(side);
      onMessage({ t: 'reveal', round: NET.round, salt: 'x', army: [{ pattern: 'block', x: zx0 + 1, y: 1, rot: 0, mirror: false }] }); }""")
    print('forged reveal ->', host.evaluate("NET.desync"), '| army accepted:', host.evaluate("NET.theirs.army !== null"))

    # The host changes the battlefield before locking in: a new round for both,
    # the guest's commitment is void.
    host.evaluate("NET.desync = ''; setCount(1000)")
    guest.wait_for_function("NET.round === 4 && S.field.count === 1000 && NET.fieldRound === 4", timeout=20000)
    print('host changed the battlefield -> guest round', guest.evaluate("NET.round"), 'count', guest.evaluate("S.field.count"),
          '| guest still locked in:', guest.evaluate("!!NET.mine"), '| host sees guest ready:', host.evaluate("S.ready[otherSide(mySide())]"))

    # Disconnect: the guest closes the tab.
    guest.close()
    host.wait_for_function("NET.status === 'lost'", timeout=40000)
    print('after guest left, host status:', host.evaluate("$('#onlineStatus').textContent"), '| new-link button shown:', host.evaluate("!$('#btnOnlineNew').hidden"))
    host.click('#btnOnlineLeave')
    host.wait_for_function("S.mode === 'duo'")
    print('left online mode ->', host.evaluate("S.mode"))
    browser.close()
