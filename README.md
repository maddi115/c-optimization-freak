# freak-kit

One small C engine, many apps. You write an app's state and rules in C, and a thin
SolidJS page. The engine does the rest: networking, syncing browsers, testing, safety.

This README is also a **playbook**: every optimization is listed once, with an ID, so you can
pull the ideas you need into a real project (see "Pick ideas for your project").

**Measured on a home PC (WSL):** a 3,000-car city with 6 browser tabs used about 1% of one CPU
core and 1.6-4 MB of RAM. A visitor downloads about 14 KB.

---

## Contents

1. [The three example apps](#the-three-example-apps)
2. [Quick start](#quick-start)
3. [The playbook](#the-playbook): every optimization, grouped by goal
   - [A. Send less over the network](#a-send-less-over-the-network)
   - [B. Use less server CPU and RAM](#b-use-less-server-cpu-and-ram)
   - [C. Run on the weakest phones and PCs](#c-run-on-the-weakest-phones-and-pcs)
   - [D. Stay correct and safe](#d-stay-correct-and-safe)
   - [E. Ship and run cheaply](#e-ship-and-run-cheaply)
4. [Lessons: bugs the tests caught](#lessons-bugs-the-tests-caught)
5. [Pick ideas for your project](#pick-ideas-for-your-project)
6. [Not built yet: the next savings](#not-built-yet-the-next-savings)
7. [Make your own app](#make-your-own-app)
8. [Tools](#tools)
9. [Files](#files)
10. [Honest limits](#honest-limits)

---

## The three example apps

| App | Sync mode | What it shows |
| --- | --- | --- |
| `apps/ride` | **Lockstep** | 3,000 cars. Browsers run the same C car code, so after a 16 KB first copy only about 8 bytes per tick travel |
| `apps/pixels` | **Rows** | A shared 64 x 64 board. Only changed squares are sent, only in the part you view. About 90 lines of C. Also the template for new apps |
| `apps/todo` | **Both** | A shared todo list. Add / tick / delete / clear are numbered actions (1-4) that every device runs itself. Text is sent once per item. A tick is one 16-byte message |

---

## Quick start

You need (Ubuntu / WSL):

```
sudo apt install build-essential clang lld nodejs npm
```

Run an app:

```
cd ~/freak-kit/apps/ride/ui && npm install && cd ~/freak-kit
make APP=ride page    # build the page and app.wasm, pre-compressed
make APP=ride run     # dev server with hot reload: http://localhost:8080
```

Use `APP=pixels` or `APP=todo` for the others (run `npm install` in their `ui/` folder first).
Look inside a running server at http://localhost:8080/debug.

---

## The playbook

Each idea has: **what it is**, **where the code is**, the **measured result**, and **when to
reuse it**. IDs (A1, B2...) stay the same, so you can refer to them in your own projects.

### A. Send less over the network

| ID | Idea | Where | Measured | Reuse when |
| --- | --- | --- | --- | --- |
| **A1** | **Lockstep: send actions, not results.** Every device runs the same C code, so only small numbered actions travel ("2 = tick item 5") and each device computes the result | `engine/main.c` (tick loop), `apps/*/wasm.c`, `apps/todo/todo.c` | Ride: about 8 bytes per tick for 3,000 cars. Todo: one 16-byte message per tick/delete | The result can be **computed** from the actions: simulations, games, lists, planners |
| **A2** | **Rows: send only what changed.** The state includes an array of fixed-size rows; the engine sends only rows that changed since each browser's last update | `engine/main.c` (`rows_diff`, `send_rows_delta`) | Pixels: a stroke sends a few bytes, not the board | Changes come from **people or the outside world**: boards, chats, posts, prices |
| **A3** | **Subscriptions: send only what each person is looking at.** A browser says "rows 0 to 2047" and gets nothing else | `sub` command in `engine/engine.def`, `main.c` | Pixels "Top half only": the bottom half costs 0 bytes | Maps, long lists, feeds: anything bigger than one screen |
| **A4** | **Mix both modes.** Actions by lockstep, text by rows, sent once | `apps/todo/app.c` | Add = 16 bytes + an 80-byte text row, once. Ticking never resends text | Apps with small changing flags and bigger fixed content |
| **A5** | **Own compressor** (`pack.h`, about 150 lines): frame-of-reference + bit-packing in blocks of 64, picks byte or 16-bit mode | `engine/pack.h` | City copy 16.4 KB (borrowed deflate: 19.1 KB). No tables, less RAM | Numeric data: positions, ids, flags. (Text compresses better with deflate) |
| **A6** | **Chunk fingerprints: repair only the broken part.** State is hashed in 1 KB chunks, 8 bytes at a time. A device that drifts gets only the broken chunks | `engine/hash.h`, `client.c`, `main.c` (`fix`, `patch`) | about 1 KB per broken chunk instead of a 48 KB full copy | Any copy of state that can drift: caches, mirrors, P2P copies |
| **A7** | **Binary messages**, JSON only for small panels | `engine/engine.def` (formats in comments) | Tick header 8 bytes | Anything sent many times per second |
| **A8** | **Pre-compressed files**: `.gz` copies made once at build time, served as-is | `Makefile` (`page`), `engine/http.c` | Page JS 10 KB, app.wasm 3.4 KB (gzipped) | Every static file. Never compress on each request |
| **A9** | **Rate limits on full copies**: one per browser per 2 s (view changes: 0.5 s) | `main.c` (`FULL_GAP`, `SUB_GAP`) | Junk "sync" spam can't flood the server | Any "send me everything" request |

### B. Use less server CPU and RAM

| ID | Idea | Where | Measured | Reuse when |
| --- | --- | --- | --- | --- |
| **B1** | **One C program, one compile** (unity build). No framework, runtime or garbage collector | `engine/unity.c` | Idle server about 1.1 MB RAM | Always |
| **B2** | **Hand-written HTTP + WebSocket**, one thread, `epoll`, one port | `engine/http.c`, `engine/ws.c` | 250 browsers: 2.5% of one core | Servers with many idle connections |
| **B3** | **Struct of arrays**: `x[]`, `y[]`, `dx[]` instead of an array of car structs, so the CPU reads in straight lines | `apps/ride/sim.h` | Enables B4 | Anything looping over many items |
| **B4** | **SIMD**: move 4+ cars per instruction | `apps/ride/sim.c` | 4.8x faster than the simple loop, same result | Hot loops over numbers |
| **B5** | **Integer-only math** (no floats in shared code) | `apps/ride/sim.c` | C and WebAssembly agree bit for bit | Any code that runs on server AND devices (A1) |
| **B6** | **Spatial grid**: "nearest cars" checks a few cells, not all cars | `apps/ride/sim.c` | Whole ride tick (3,000 cars + grid + checks): 15.5 us | Maps, "nearby", collision checks |
| **B7** | **Fixed memory**: all app state in one block, no `malloc` while running | `engine/engine.h` (`state_size`) | Snapshots and hot reload are one `memcpy` | Always: easy to save, replay and debug |
| **B8** | **Block diff**: compare rows 256 bytes at a time before row by row | `main.c` (`rows_diff`) | Pixels tick 12 us -> 0.14 us | Large arrays that rarely change |
| **B9** | **Send queue per connection** + "fell behind" limit: a slow browser gets a fresh copy instead of an endless backlog | `engine/ws.c`, `main.c` (`BEHIND`) | RAM stays flat (tested: 6 background tabs held at 4,004 KB) | Any server with slow clients |

### C. Run on the weakest phones and PCs

| ID | Idea | Where | Measured | Reuse when |
| --- | --- | --- | --- | --- |
| **C1** | **One copy of the browser logic, in C** (`client.c`), run as WebAssembly in the page and natively in tests | `engine/client.c`, `engine/wasm_glue.h` | Tests check the exact code browsers run | Logic you'd otherwise write twice (C and JS) |
| **C2** | **WebAssembly 1.0 only**, no imports, no libc | `Makefile` (`-mcpu=mvp -nostdlib`) | Any browser since 2017. app.wasm 3-4 KB gzipped | Shipping C to browsers |
| **C3** | **JavaScript built for 2017 browsers**, no `DecompressionStream` | `apps/*/ui/vite.config.ts` (`es2017`) | Old browsers work | Always, unless you need a newer feature |
| **C4** | **Draw only when needed**: the animation loop stops when nothing is waiting | `engine/js/tunnel.ts` (`schedule`) | Idle page uses almost no CPU | Every page |
| **C5** | **Jitter buffer + gliding** that switches itself off on slow devices | `tunnel.ts`, `apps/ride/ui/src/MapCanvas.tsx` | 60 / 60 / 51 fps on fast / mid / 6x-slower phones | Anything that moves on screen |
| **C6** | **Redraw only on real change**: a change counter in C, checked by the page | `apps/todo/todo.c` (`changes`), `tunnel.ts` (`onState`) | Ticks with no change cost the page nothing | Lists, panels, dashboards |
| **C7** | **Read straight from wasm memory**: the page makes views into C's arrays, nothing copied | `apps/ride/ui/src/connection.ts`, `apps/todo/ui/src/App.tsx` | 3,000 cars drawn without copying | Big arrays drawn often |
| **C8** | **SolidJS for the UI**: no virtual DOM. TypeScript and Vite run only while building | `apps/*/ui` | Page JS 7.7-10 KB gzipped | Kept on purpose; a custom mini version would save about 6 KB |

### D. Stay correct and safe

These cost nothing while running, and they are what makes the optimizations safe to keep.

| ID | Idea | Where | Measured | Reuse when |
| --- | --- | --- | --- | --- |
| **D1** | **Chaos test**: simulated browsers with lag, disconnects, junk, slow readers and memory corruption, faster than real time | `main.c` (`--chaos`) | about 400-12,000x real time. Found 3 real sync bugs (see Lessons) | Any sync or network code |
| **D2** | **Record + replay**: every input is logged; replay must give identical checksums | `main.c` (`--replay`) | MATCH on all three apps | Debugging "it happened once" bugs |
| **D3** | **Replay viewer**: scrub a recording in the real page | `main.c` (`--viewer`), `/viewer` | Jump to any tick | Explaining or demoing what happened |
| **D4** | **App rules checked** every second live, every tick in tests; a failure saves `crash.snap` | `check()` in each `app.c` | | Always: cheap, catches bugs early |
| **D5** | **Fuzzing** with sanitizers | `engine/fuzz/` | Millions of random inputs into HTTP, WebSocket, `client.c`, the unpacker and app commands: 0 crashes | Any parser that reads outside data |
| **D6** | **Sandbox**: seccomp + Landlock. The server can't read outside its folder, start programs or open connections | `engine/sandbox.c` | Tested: reading `/etc/passwd` is denied | Any public server |
| **D7** | **Protocol from one file**: `.def` generates C and TypeScript | `engine/tools/gen_protocol.c` | C and UI can't disagree on a message | Any two-language project |

### E. Ship and run cheaply

| ID | Idea | Where | Measured | Reuse when |
| --- | --- | --- | --- | --- |
| **E1** | **One static file** with the page baked in | `make release`, `engine/tools/embed.c` | about 1.05-1.07 MB. Runs from an empty folder | Deploys to tiny servers, SD cards, USB sticks |
| **E2** | **Hot reload**: rebuild the app code; the running server swaps it in, keeping state and connections | `main.c` (`load_app`) | Fare changed +$10.00 live, no restart | Fast development |
| **E3** | **Snapshots**: state saved every second, restored on start | `main.c` (`state.snap`) | Todo list survives a restart | Small apps with no database |
| **E4** | **HTTPS by Caddy**, systemd service, optional ~1 MB Docker image | `deploy/` | Tested over HTTP/2 + `wss://` | Going live |

---

## Lessons: bugs the tests caught

Keep these in mind in any project that syncs state.

| Bug | Found by | Fix | Lesson |
| --- | --- | --- | --- |
| A browser joining between ticks got an action twice (fine for "set", wrong for "flip") | Chaos, with the todo app: 35 divergences | Full copies and patches are sent from the state as of the last tick (`after_step`) | Test with **non-repeatable actions** (toggle, add), not only "set" actions |
| Rows sync skipped changes made in the same tick | Chaos | Stamp changed rows with `ticks + 1` | Off-by-one in "what changed since" is the classic sync bug |
| Repair gave up forever after a few failures | Chaos | Failure counter resets after 200 ticks | Every "give up" counter needs a way back |
| Replay cut commands at the first space | Replay of the todo app | Recordings keep the whole line | Test with real-looking data (spaces, long text) |
| Switching view froze a tab for up to 2 s | Browser test (two tabs) | Separate 0.5 s limit for view changes | Spam limits must not punish normal clicks |
| Hot reload kept old code | Hot reload test | Unique file name per load | `dlopen` caches by name |

---

## Pick ideas for your project

| Your project has... | Take these | Example |
| --- | --- | --- |
| Shared lists, flags, game state | **A1, A4, B5, C6, D1** | `apps/todo` |
| A map with "nearby" things | **A2, A3, B6, C7** (+ N5 below) | geo / hyperlocal apps |
| Posts, chat, feeds from people | **A2, A3, A9, D5** | `apps/pixels` |
| Many things moving on screen | **A1, B3, B4, C5, C7** | `apps/ride` |
| Images | **A8** (+ N2, N4 below) | |
| Payments | (+ N1 below) | |
| A tiny or cheap server | **B1, B2, B9, E1** | runs on about 1 MB |
| Users on old or cheap phones | **C2, C3, C4, C5** | 51 fps at 6x slower CPU |
| Anything public | **D5, D6, E4** | |

---

## Not built yet: the next savings

Ranked by how much each saves (estimates for about 1 million users a month). The first three hit
the three biggest costs:

| Cost rank | Cost | About per month |
| --- | --- | --- |
| #1 | Stripe fees | about $400 per 1,000 paid $3 posts |
| #2 | Image views (storage reads) | about $32 |
| #3 | Bigger server (RAM per user) | about EUR 10-20 |
| #4 | Domain | about $1 |
| #5 | Image storage | under $1 |

| ID | What to build in C | Hits cost # | Saves |
| --- | --- | --- | --- |
| **N1** | **A credits ledger.** The C server keeps each user's credit balance and signs each post as "paid". One Stripe charge, like $10, covers several posts | #1 Stripe | 3 of every 4 fixed 30-cent fees (90 cents per bundle of 4 posts) |
| **N2** | **Serve images from the C server itself**, with Cloudflare's free cache in front. Skip R2: the server's disk holds the images, and Hetzner includes 20 TB of traffic a month | #2 image views | most of the about $32 |
| **N3** | **The buffer fix**: smaller send buffers per user (about 35 KB instead of 260 KB) | #3 server size | stays on the cheapest server |
| **N4** | **An image pipeline on the server**: resize, encode JPEG XL (the library has a C interface), store raw bytes instead of base64, and skip duplicate images by fingerprint (`hash.h`) | #2 and #5 | posters skip the 514 KB encoder download; images about 25% smaller |
| **N5** | **Only send posts inside the visible map area.** Posts become rows grouped by map area, using the "subscribe to part of the rows" feature (A3, like pixels' "Top half only") | bandwidth | phones get only nearby posts, not the whole world's |
| **N6** | **Skip empty ticks**: send nothing while nothing happens | bandwidth | about 160 bytes/s per idle user |
| **N7** | **Smaller actions**: 8 bytes -> 2-3 bytes for common ones | bandwidth | about half of action traffic |
| **N8** | **Tiny blurred placeholders** (ThumbHash) made in C | feel and speed | about 30 bytes shown while the real image loads |

C can't lower Stripe's 30-cent price per charge. It can only make each charge cover more posts (N1).

---

## Make your own app

```
make new NAME=myapp
cd apps/myapp/ui && npm install && cd ../../..
make APP=myapp page run
```

An app is 4 things:

1. **`app.c`**: your state struct and the functions in `engine/engine.h` (init, step, command, check...)
2. **`protocol.def`**: your messages. `make` turns it into C and TypeScript
3. **`ui/`**: your page. It talks to the engine through `src/engine/tunnel.ts`
4. **`wasm.c`** (lockstep apps only): your shared rules, built for the browser

Which sync mode?

| Your app's changes come from... | Use |
| --- | --- |
| Code (simulations, games, list actions) | **Lockstep** (A1) |
| People or the outside world (text, posts, prices) | **Rows** (A2) |
| Both | **Both** (A4) |

Rule for lockstep actions: write them so the **same C file** runs on the server and the device,
with integer math only (B5). Test with `make APP=myapp chaos`.

---

## Tools

Run as `make APP=name <tool>`.

| Tool | What it does |
| --- | --- |
| `run` | Dev server. Records inputs and saves state every second |
| `app` | Rebuild only the app code; the running server swaps it in |
| `replay` | Re-run the last session; every checksum must match |
| `viewer` | Scrub the last session in the real page: http://localhost:8080/viewer |
| `chaos` | Simulated browsers with lag, junk and corruption. Must end in PASSED |
| `fuzz` | Random inputs with sanitizers (`FUZZ_SECONDS=60` to run longer) |
| `bench` | Time the ticks |
| `release` | One static file: `build/APP/APP` |

---

## Files

```
freak-kit/
  Makefile              every tool, for any app: make APP=name ...
  engine/               never edited per app
    engine.h            THE contract: what an app provides
    engine.def          the engine's own messages (generates C + TypeScript)
    main.c              tick loop, both sync modes, healing, record/replay, viewer, chaos, /debug, hot reload
    client.c            the browser half, in C (WebAssembly in the page, native in chaos)
    wasm_glue.h         what the page can call in app.wasm (no libc, no imports)
    client_wasm.c       app.wasm for rows-only apps
    pack.h              our own compressor
    hash.h              the fast fingerprint, identical in C and WebAssembly
    ws.c ws.h           WebSocket with a send queue per connection
    http.c http.h       the page from memory, pre-compressed copies
    sandbox.c           seccomp + Landlock lockdown
    unity.c             the whole server is one compile of this file
    js/tunnel.ts        thin JS: opens the tunnel, feeds app.wasm
    tools/              gen_protocol.c (.def -> C + TS), embed.c (page -> C)
    fuzz/               fuzz_http.c, fuzz_ws.c, fuzz_client.c, fuzz_app.c
  apps/
    ride/               lockstep: sim.c rides.c app.c wasm.c protocol.def ui/
    pixels/             rows, and the template: app.c protocol.def ui/
    todo/               both: todo.c (shared rules) app.c wasm.c protocol.def ui/
  deploy/               Caddyfile, freak-kit.service, Dockerfile
```

---

## Honest limits

- **An empty tick still costs 8 bytes, 20 times a second** per user (N6 fixes it).
- **Each connected user can take up to about 260 KB of RAM** in send buffers (N3 fixes it).
- **Lockstep fits changes that are computed.** Real-world data (GPS, prices, text) belongs in rows.
- **Rows mode scans for changes every tick** (in blocks, so it's cheap). For millions of rows, the
  app should mark changed rows itself.
- **The viewer replays from the start on a backward jump.** Fine for minutes; hours would need checkpoints.
- **The C code is fuzzed and sandboxed but not professionally audited.** Keep Caddy in front.
