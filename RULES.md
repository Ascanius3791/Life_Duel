# Life Duel — Rules

Life Duel is a two-player battle built on Conway's Game of Life. Each player
builds an army of living cells, and then the simulation decides who survives.
Nobody gets to act once the battle has started.

---

## 1. The board

```
 ┌────────────┬──────────────────────────────────────┬────────────┐
 │            │                                      │            │
 │    RED     │        neutral battlefield           │    BLUE    │
 │   area     │        (grey cells live here)        │    area    │
 │  1/5 wide  │              3/5 wide                │  1/5 wide  │
 │            │                                      │            │
 └────────────┴──────────────────────────────────────┴────────────┘
```

* The board is a rectangle (Small 200×100, Medium 260×128 or Large 360×176 cells).
* **Red** owns the left fifth and **Blue** the right fifth. You may only place
  your own cells inside your own area.
* The middle holds the **neutral grey** cells of the chosen battlefield.
* Everything outside the board counts as permanently dead. The board does
  **not** wrap around.

## 2. Preparation

1. Choose a **battlefield**. Every preset except *Random (unfair!)* is
   symmetric, so neither side has an advantage:
   * *Random (mirrored)* / *Random (rotational)*: a chosen number of random grey cells
   * *Reactor core*: a dense soup in the centre that explodes before your armies arrive
   * *Bunker line*, *Hive lattice*, *Blinker field*, *Pulsar towers*,
     *Pentadecathlon gates*, *Fortress*: structured obstacles
   * *No man's land*: nothing at all
2. Both players get the same number of **points** (default 120).
3. Buy patterns from your **arsenal** and drop them into your area:

   | Pattern | Cost | | Pattern | Cost |
   |---|---:|---|---|---:|
   | Single cell | 1 | | Glider | 5 |
   | Block | 4 | | Lightweight spaceship | 9 |
   | Blinker | 3 | | Middleweight spaceship | 11 |
   | Beehive | 6 | | Heavyweight spaceship | 13 |
   | Eater 1 | 7 | | Copperhead | 28 |
   | R-pentomino | 5 | | Pentadecathlon | 12 |
   | Diehard | 7 | | Pulsar | 48 |
   | Acorn | 7 | | Gosper glider gun | 36 |
   | | | | Simkin glider gun | 36 |

   A pattern costs one point per live cell.

   Every moving pattern in the arsenal starts out flying (or shooting) towards
   the opponent: Red's face east, Blue's are mirrored to face west. You can
   rotate and mirror them freely.
   Your arsenal opens **over your opponent's area**, so it never covers the
   area you are building in. Only one arsenal is open at a time, and it can
   be folded away to a small tab at the board edge (`A`).
4. With **blind placement** (on by default) Red places first while Blue looks
   away, then Blue places while Red's area is hidden. Neither player sees the
   other's army until the battle starts. With blind placement off, both
   players place at the same time and see everything.

## 3. Life as usual…

Every generation, all cells update at the same time. Each cell looks at its
eight neighbours. All colours count as "alive":

* A **living** cell with **2 or 3** living neighbours **survives**. Otherwise it dies.
* A **dead** cell with **exactly 3** living neighbours is **born**.

## 4. …but with colours

A **newborn cell takes the colour that contributed more parents**:

* Count the red and the blue cells among its three parents.
* More red than blue → **red**. More blue than red → **blue**.
* Equal (a tie) → **grey**.
* Grey parents help a cell to be born, but they **never vote**.

| Parents | Newborn |
|---|---|
| 🟥 🟥 🟥 | 🟥 red |
| 🟥 🟥 🟦 | 🟥 red (2 vs 1) |
| 🟥 ⬜ ⬜ | 🟥 red (1 vs 0) |
| 🟥 🟦 ⬜ | ⬜ grey (1 vs 1) |
| ⬜ ⬜ ⬜ | ⬜ grey (0 vs 0) |
| 🟦 🟦 ⬜ | 🟦 blue |

Example: the `·` cell has exactly three living neighbours (two red, one blue),
so it is born **red**:

```
 R . .
 R · .
 . B .
```

### Rule variants for surviving cells

You choose the variant in **Settings**:

* **Birthright** (default): a surviving cell **keeps its colour** for as long as
  it lives. Only births are decided by the vote.
* **Contagion**: every surviving cell **votes again each generation** using the
  same rule, but now with its living neighbours (2 or 3 of them). Ties turn the
  cell grey.
  * A red cell with 2 blue neighbours turns **blue**.
  * A red cell with 1 red and 1 blue neighbour turns **grey**.
  * A red cell whose only neighbours are grey also turns **grey**. Grey
    obstacles "infect" whatever touches them.

## 5. Winning

The battle ends as soon as one of these happens:

1. **Elimination**: one colour has no cells left, and the other colour wins.
   If both disappear in the same generation, it's a draw. A colour can never
   come back once it is gone, because a birth needs at least one parent of
   that colour to win the vote.
2. **Repetition**: the entire board is exactly the same as in an earlier
   generation. From then on the game would loop forever. The **average**
   number of counted red and blue cells over one full cycle decides, so
   oscillators are judged fairly. A frozen board is a cycle of length 1.
3. **Generation limit**: the colour with more counted cells wins. Equal
   counts are a draw. By default there is **no limit (∞)**: the battle runs
   until one army is wiped out or the board repeats. A limit (for example
   2000 generations) can be set in Settings.

**Home cells don't count** (on by default, can be switched off in Settings):
red cells inside Red's own starting area and blue cells inside Blue's are
left out of the counts for rules 2 and 3. Only cells that have left home, or
were born outside it, score. Cells inside the *opponent's* area do count.
Elimination (rule 1) still looks at every cell: an army that is wiped out
loses, even if the winner's cells all stayed at home.

The bots were trained with all cells counted and a limit of 2000
generations. In vs Bot and Watch mode both settings can still be changed.

> Rules are perfectly symmetric. Two mirror-image armies on a symmetric
> battlefield therefore always end in a draw. You have to be different to win!

## 6. Controls (browser)

| Action | How |
|---|---|
| Place a pattern | Drag a card onto your area, **or** click a card and then click the board as often as you like |
| Stop placing | `Esc` or right-click |
| Rotate | `R` / `Shift+R`, the mouse wheel while placing, or ↺ ↻ |
| Mirror | `F` (left/right) or `V` (up/down), or ⇋ ⇅ |
| Remove a placed pattern | Right-click it (you get the points back) |
| Undo last placement | `Ctrl+Z` |
| Play / pause | `Space` |
| Single generation | `→` |
| Speed | Drag the speed slider (1–1000 generations per second), mouse wheel over it, `+` / `−` |
| Maximum speed | **⚡ Max** or `M`: as fast as the C++ engine can compute, rendering frames as they come |
| Jump to the result | **⏭** |
| Fold / unfold the arsenal | `A`, the » / « button, or the tab at the board edge |
| Focus mode (once both armies are ready) | **⛶ Focus** or `F`: the board fills the screen, controls fade out while the mouse rests. `Esc` leaves it |
| Play against the bot | **vs Bot** at the top left: pick your side, the battlefield is the one the bot was trained for |
| Rules | `H` |

---

*Planned: an interactive tutorial that shows, cell by cell, when and why cells
are born, die, and change colour.*
