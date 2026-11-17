# freak-kit

One small C engine, many apps. You write an app's state and rules in C, and a thin
SolidJS page. The engine gives you everything else:

- HTTP and WebSocket on one port, from one static ~1 MB file
- Two ways to keep browsers up to date (lockstep or rows)
- Record and replay, chaos testing and fuzzing
- Hot reload, a `/debug` page, and a seccomp + Landlock sandbox

Two example apps are included:

| App | Sync mode | What it shows |
| --- | --- | --- |
| `apps/ride` | **Lockstep** | 3,000 cars. The browser runs the same C car code (`app.wasm`). After a 19 KB first copy, about 8 bytes per tick. |
| `apps/pixels` | **Rows** | A shared 64 x 64 board. Only changed squares are sent, and only in the part you view. About 90 lines of C. It is also the template for new apps. |

## What you need (Ubuntu / WSL)

```
sudo apt install build-essential clang lld nodejs npm
```

Optional: `sudo apt install caddy` for HTTPS.

## Run an app

```
cd ~/freak-kit/apps/ride/ui && npm install && cd ~/freak-kit
make APP=ride page    # build the page (and app.wasm), pre-compressed
make APP=ride run     # dev server with hot reload: http://localhost:8080
```

Use `APP=pixels` for the other app. Look inside at http://localhost:8080/debug.

## Make your own app

```
make new NAME=myapp
cd apps/myapp/ui && npm install && cd ../../..
make APP=myapp page run
```

An app is 4 things:

1. **`app.c`**: your state struct and the functions listed in `engine/engine.h`
   (init, step, command, rules to check...)
2. **`protocol.def`**: your messages. `make` turns it into C and TypeScript.
3. **`ui/`**: your page. It uses `src/engine/tunnel.ts` to talk to the engine.
4. **`wasm.c`** (lockstep apps only): your step code, built for the browser.

### Which sync mode?

| Your app's changes come from... | Use | Why |
| --- | --- | --- |
| Code: simulations, games, planners | **Lockstep** | The browser can compute it, so only inputs are sent |
| People or the outside world: boards, chats, dashboards, stores | **Rows** | Can't be predicted. Send only changed rows, only in the part each browser views |
| Both | Both | An app can set both `lockstep_bytes` and `rows` |

## Tools (`make APP=... <tool>`)

| Tool | What it does |
| --- | --- |
| `run` | Dev server. Records inputs (`last-run.*`) and saves state (`state.snap`) every second |
| `app` | Rebuild only the app code. The running server swaps it in, keeping all state and connections |
| `replay` | Re-run the last session offline. Every checksum must match, and the app's rules are checked every tick |
| `viewer` | Scrub through the last session **in your real app page**: open `/viewer`, drag the slider, step tick by tick, press Play |
| `chaos` | Simulated browsers with lag spikes, disconnects, junk commands, slow readers and memory corruption, about 500x faster than real time. Must end in PASSED |
| `fuzz` | libFuzzer + AddressSanitizer on the HTTP parser, the WebSocket parser, the browser code (`client.c` + unpacker) and the app's commands |
| `bench` | Time the ticks |
| `release` | `build/APP/APP`: ONE static file with the page baked in. Copy it anywhere and run it |

## Deploy

- **Leanest:** copy the release file to a Linux server and use `deploy/freak-kit.service`
  (systemd). Put Caddy in front for HTTPS with `deploy/Caddyfile`.
- **Optional:** `deploy/Dockerfile` makes a ~1 MB image `FROM scratch`.

## Before and after this round ("works on the weakest PC")

| # | Before | Now |
| --- | --- | --- |
| 1 | Borrowed deflate compressor (1,420 lines) | **Our own compressor** (`pack.h`, ~150 lines). Smaller on this data: city copy 16.4 KB vs 19.1 KB. No tables, so less RAM |
| 2 | Browser logic written twice (C for chaos, TypeScript for the page) | **One copy in C** (`client.c`). The page runs it as WebAssembly, chaos runs it natively. Chaos now tests the exact code browsers run |
| 3 | Browser needed `DecompressionStream` (Chrome 80+, Safari 16.4+) | **Not needed.** Our unpacker is inside `app.wasm` |
| 4 | WebAssembly used newer CPU features | **WebAssembly 1.0 only** (`-mcpu=mvp`), no imports. Runs in any browser since 2017 |
| 5 | JavaScript built for new browsers | **Built for 2017 browsers** (`es2017`) |
| 6 | Animation loop ran every frame | **Runs only while there is something to show** |
| 7 | Rows changes checked row by row | **Checked in blocks of 256 bytes first.** Pixels tick: 12 us -> 0.14 us |
| 8 | No way to look inside a recording | **Replay viewer** in the real page, with the inputs shown next to each tick |
| 9 | View change waited up to 2 s | **Half a second at most**, still spam-proof (found by the browser test this round) |
| 10 | Browser code not fuzzed | **Fuzzed:** 1.17 million random server messages into `client.c` and the unpacker. 0 crashes |

**Kept on purpose:** SolidJS (your UI), TypeScript and Vite. TypeScript and Vite only run on
your PC while building. Browsers never download them, so they cost the weakest PC nothing.
TypeScript also catches C/UI mismatches, because its types come from the same `.def` files.

## Numbers (measured)

| | Last round | Now |
| --- | --- | --- |
| City copy on join | 19.2 KB | **16.4 KB** |
| Car updates | ~8 bytes per tick | ~8 bytes per tick |
| Full tick | ride 16.3 us / pixels 12 us | **ride 15.5 us / pixels 0.14 us** |
| app.wasm (gzip) | ride ~3.5 KB | ride 3.4 KB / pixels 2.9 KB (one shared engine half) |
| Page JS (gzip) | | ride 10.0 KB / pixels 7.6 KB |
| Release file | 1.08 / 1.05 MB | 1.07 / 1.05 MB |
| Server RAM | ~2.3 / 2.0 MB | **1.46 / 1.07 MB** |
| Fast / mid / cheap phone | 60 / 60 / 51 fps | 60 / 60 / 51 fps, JS heap 3.9 MB |
| Chaos | PASSED | PASSED, now with the real browser code |

## Files

```
freak-kit/
  Makefile              every tool, for any app: make APP=name ...
  engine/               never edited per app
    engine.h            THE contract: what an app provides
    engine.def          the engine's own messages (generates C + TypeScript)
    main.c              tick loop, both sync modes, healing, record/replay, viewer, chaos, /debug, hot reload
    client.c            the browser half, in C: runs as WebAssembly in the page and natively in chaos
    wasm_glue.h         what the page can call in app.wasm (no libc, no imports)
    client_wasm.c       app.wasm for apps that only use rows
    pack.h              our own compressor
    ws.c ws.h           WebSocket with a send queue per connection
    http.c http.h       the page from memory, pre-compressed copies
    sandbox.c           seccomp + Landlock lockdown
    hash.h              the fast fingerprint, identical in C and WebAssembly
    unity.c             the whole server is one compile of this file
    js/tunnel.ts        thin JS: opens the tunnel, feeds app.wasm (copied into each app's ui/src/engine/)
    tools/              gen_protocol.c (.def -> C + TS), embed.c (page -> C)
    fuzz/               fuzz_http.c, fuzz_ws.c, fuzz_client.c, fuzz_app.c
  apps/
    ride/               lockstep example: sim.c rides.c app.c wasm.c protocol.def ui/
    pixels/             rows example and template: app.c protocol.def ui/
  deploy/               Caddyfile, freak-kit.service, Dockerfile
```

## Honest limits

- **Lockstep fits apps whose changes are computed.** Real-world data (GPS, prices) belongs in rows.
- **Rows mode checks every row for changes every tick** (in blocks, so it's cheap). For millions
  of rows, the app should mark changed rows itself.
- **Each message must fit the 256 KB per-connection send queue.** Big row sets are sent in
  pieces automatically. A lockstep state must pack below that.
- **The viewer re-runs the recording from the start on a backward jump.** Instant for minutes
  of recording; for hours it would want saved checkpoints.
- **The C code is fuzzed and sandboxed but not professionally audited.** Keep Caddy in front.
