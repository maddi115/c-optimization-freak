# freak-kit: one engine, many apps.   make APP=ride ...   (default APP=ride)
#
#   make page       build the app's page (+ app.wasm if the app has wasm.c), pre-compressed
#   make run        dev server with hot reload            http://localhost:8080
#   make app        rebuild only the app code; the running server swaps it in
#   make replay     re-run the last session, compare checksums
#   make viewer     scrub through the last session in the real app page (/viewer)
#   make chaos      simulated users, lag, disconnects, junk, corruption (must say PASSED)
#   make bench      time the ticks
#   make fuzz       throw millions of random inputs at the parsers (needs clang)
#   make release    ONE static file: build/APP/APP, page baked in
#   make new NAME=x start a new app from the template
APP     ?= ride
A        = apps/$(APP)
B        = build/$(APP)
UI       = $(A)/ui
CC      ?= cc
CFLAGS   = -O2 -march=native -Wall -Wextra
WASMCC  ?= clang
E        = engine
ENGINE   = $(E)/unity.c $(E)/main.c $(E)/ws.c $(E)/ws.h $(E)/http.c $(E)/http.h $(E)/sandbox.c $(E)/sandbox.h \
           $(E)/engine.h $(E)/hash.h $(E)/gen/engine_proto.h
APPSRC   = $(wildcard $(A)/*.c $(A)/*.h) $(A)/gen/protocol.h

dev: $(B)/server $(B)/libapp.so

# ---- protocol: .def -> C header + TypeScript, for the engine and for the app
build/tools/gen_protocol: $(E)/tools/gen_protocol.c
	@mkdir -p build/tools
	$(CC) -O2 -Wall -Wextra -o $@ $<
build/tools/embed: $(E)/tools/embed.c
	@mkdir -p build/tools
	$(CC) -O2 -Wall -Wextra -o $@ $<
$(E)/gen/engine_proto.h: $(E)/engine.def build/tools/gen_protocol
	@mkdir -p $(E)/gen
	./build/tools/gen_protocol $< $@ /dev/null ENG
$(A)/gen/protocol.h: $(A)/protocol.def build/tools/gen_protocol
	@mkdir -p $(A)/gen
	./build/tools/gen_protocol $< $@ $(UI)/src/protocol.ts APP
$(UI)/src/engine/engine_proto.ts: $(E)/engine.def build/tools/gen_protocol
	@mkdir -p $(UI)/src/engine
	./build/tools/gen_protocol $< /dev/null $@ ENG
$(UI)/src/engine/tunnel.ts: $(E)/js/tunnel.ts
	@mkdir -p $(UI)/src/engine
	cp $< $@
protocol: $(E)/gen/engine_proto.h $(A)/gen/protocol.h $(UI)/src/engine/engine_proto.ts $(UI)/src/engine/tunnel.ts

# ---- dev build: engine binary + app as a hot-reloadable library
$(B)/server: $(ENGINE)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ $(E)/unity.c -ldl -lm
app: $(B)/libapp.so
$(B)/libapp.so: $(APPSRC) $(E)/engine.h $(E)/hash.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -fPIC -shared -o $(B)/libapp.tmp.so $(A)/app.c -lm
	mv $(B)/libapp.tmp.so $@

# ---- page
# app.wasm: the app's wasm.c (lockstep apps), or the engine's rows-only client. Oldest WebAssembly
# level (mvp) and no C library, so every browser since 2017 runs it.
WASMSRC  = $(if $(wildcard $(A)/wasm.c),$(A)/wasm.c,$(E)/client_wasm.c)
wasm: protocol
	@mkdir -p $(UI)/public
	$(WASMCC) --target=wasm32 -mcpu=mvp -O2 -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -I$(E) -I$(A) \
	  -Wl,--no-entry -Wl,--strip-all -o $(UI)/public/app.wasm $(WASMSRC)
	@ls -l $(UI)/public/app.wasm

page: protocol wasm
	cd $(UI) && npm run build
	find $(UI)/dist -type f \( -name '*.js' -o -name '*.html' -o -name '*.css' -o -name '*.wasm' \) -exec gzip -9 -k -f {} \;

# ---- run and test (state files live in build/APP/)
run: dev
	cd $(B) && ./server --dist ../../$(UI)/dist
replay: dev
	cd $(B) && ./server --replay
viewer: dev
	cd $(B) && ./server --viewer --dist ../../$(UI)/dist
chaos: dev
	cd $(B) && ./server --chaos 20 2000
bench: dev
	cd $(B) && ./server --bench 5000

# ---- one static file
release: page build/tools/embed
	./build/tools/embed $(UI)/dist $(B)/assets.c
	$(CC) -O2 -Wall -Wextra -DRELEASE -DAPP_FILE='"../$(A)/app.c"' -DASSETS_FILE='"../$(B)/assets.c"' \
	  -I$(E) -static -o $(B)/$(APP) $(E)/unity.c -lm
	@ls -l $(B)/$(APP)

# ---- fuzzing (clang + libFuzzer): each runs for FUZZ_SECONDS
FUZZ_SECONDS ?= 60
FUZZ = clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -I$(E)
fuzz: protocol
	@mkdir -p build/fuzz/corpus-http build/fuzz/corpus-ws build/fuzz/corpus-client build/fuzz/corpus-$(APP)
	$(FUZZ) -o build/fuzz/http $(E)/fuzz/fuzz_http.c
	$(FUZZ) -o build/fuzz/ws $(E)/fuzz/fuzz_ws.c
	$(FUZZ) -o build/fuzz/client $(E)/fuzz/fuzz_client.c
	$(FUZZ) -DAPP_FILE='"../../$(A)/app.c"' -o build/fuzz/$(APP) $(E)/fuzz/fuzz_app.c -lm
	./build/fuzz/http -max_total_time=$(FUZZ_SECONDS) -print_final_stats=1 build/fuzz/corpus-http 2>&1 | tail -4
	./build/fuzz/ws -max_total_time=$(FUZZ_SECONDS) -print_final_stats=1 build/fuzz/corpus-ws 2>&1 | tail -4
	./build/fuzz/client -max_total_time=$(FUZZ_SECONDS) -print_final_stats=1 build/fuzz/corpus-client 2>&1 | tail -4
	./build/fuzz/$(APP) -max_total_time=$(FUZZ_SECONDS) -print_final_stats=1 build/fuzz/corpus-$(APP) 2>&1 | tail -4

# ---- start a new app from the smallest example
new:
	@test -n "$(NAME)" || (echo "usage: make new NAME=myapp" && false)
	@test ! -e apps/$(NAME) || (echo "apps/$(NAME) already exists" && false)
	mkdir -p apps/$(NAME)
	cp apps/pixels/app.c apps/pixels/protocol.def apps/$(NAME)/
	(cd apps/pixels/ui && tar --exclude=node_modules --exclude=dist -cf - .) | (mkdir -p apps/$(NAME)/ui && cd apps/$(NAME)/ui && tar xf -)
	@echo "apps/$(NAME) created (a copy of pixels). Next: cd apps/$(NAME)/ui && npm install && cd ../../.. && make APP=$(NAME) page run"

clean:
	rm -rf build $(E)/gen apps/*/gen

.PHONY: dev protocol app wasm page run replay viewer chaos bench release fuzz new clean
