# QUBE - Queue-Based Unified Balancing Engine

C++17 scheduling engine + raylib isometric 3D waiting-room simulation.

## One-time setup (Windows, PowerShell)
    winget install Git.Git
    winget install Kitware.CMake
    winget install BrechtSanders.WinLibs.POSIX.UCRT
Close and reopen VS Code / the terminal, then check: `g++ --version`, `cmake --version`, `git --version`.
VS Code extensions: "C/C++" and "CMake Tools" (Microsoft).

## Build and run
    cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    .\build\qube_tests.exe     # engine tests, should print "All engine tests passed."
    .\build\qube.exe           # the simulation

The first configure downloads raylib (needs internet, ~1 minute).
If you use Visual Studio Build Tools (MSVC) instead: drop `-G ...`, build with
`cmake --build build --config Release`, run `.\build\Release\qube.exe`.

## Controls
Bottom dock: Start / Pause / Resume, Reset, speed slider (0.25x-8x), Auto-arrivals toggle,
and "+ Add customer" (choose priority 1-3, service time 1-60 s, quantity 1-10).
Keys: SPACE start/pause/resume | A random add | B rush (+6) | UP/DOWN speed | 1-4 policy | TAB overview/fairness panel | G auto-arrivals | R reset | F fullscreen
Lanes and counters: customers wait in three lanes (P3 | P2 | P1) and never change lane or get re-sorted.
The four counters start as P3, P2, P1, P1 ("home" class, shown on each counter sign and panel card);
click a P-badge to cycle its class (P1 -> P2 -> P3). A free counter takes its home lane first, borrows from the
other lanes when its own is empty, and any lane front that has waited >= 6 s (aging override) can be taken by any counter.
Policies (top bar or keys 1-4):
  FCFS     oldest lane front overall, ignores lanes and home classes (comparison baseline)
  PRIORITY home lane first; when empty borrow the highest class; no aging (P1 can starve if it has no home counter)
  SJF      shortest service time among everyone waiting, ignores home classes (comparison baseline)
  AGING    default: home lane first + borrowing by aging score + 6 s aging override

## Settings (top bar "Settings" button, or key S)
Arrivals: average arrival interval, rush frequency and size, shortest/longest service time, P3 and P2 share of arrivals.
Scheduling: aging override wait, aging interval, starvation threshold.
Counters: number of counters (2-6, restarts the run) and walk-to-desk time.
Shift+click changes a value by 5 steps. Values are saved to qube_settings.txt in the folder you launch the program from
(delete that file to go back to defaults, or use "Reset to defaults").

## Layout
- `src/engine/` scheduling core, no graphics, written in beginner-friendly C++ (plain classes, loops, small helper functions). Rendering only reads it.
- `src/main.cpp` raylib renderer, characters, HUD.
- `tests/engine_tests.cpp` engine tests.

## Roadmap
1. DONE: engine (FCFS) + visual prototype + basic live stats
2. DONE: on-screen controls, speed slider, add-customer form
3. DONE: Priority, SJF, Aging policies + selector + per-priority wait stats + starvation count
4. DONE: fairness tab (Jain index, wait spread, p90, per-class starvation, counter balance, queue chart); least-used-counter dispatch
5. DONE: three priority lanes, counters with home classes (click to change), borrowing + aging override
6. DONE: on-screen settings window (saved between runs)
7. Visual polish (hover-only labels, camera pan/zoom)
