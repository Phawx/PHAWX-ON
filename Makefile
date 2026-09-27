CC      ?= x86_64-w64-mingw32-gcc
WINDRES ?= x86_64-w64-mingw32-windres
ifeq ($(CC),cc)
CC = x86_64-w64-mingw32-gcc
endif

VERSION := $(shell sed -n 's/^\#define PH_VERSION *"\(.*\)"/\1/p' src/version.h)

CFLAGS  = -Os -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -Wno-cast-function-type -ffunction-sections \
          -fno-stack-protector -flto=auto -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Isrc -Iplugins/sdk
LDFLAGS = -s -flto=auto -mwindows -municode -Wl,--gc-sections -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va
LIBS    = -luser32 -lgdi32 -lshell32 -ladvapi32 -lpowrprof -lhid -lsetupapi -ldwmapi -lole32 -loleaut32 -luuid -lmsimg32 -lversion

SRC  = $(wildcard src/*.c src/hw/*.c src/cpu/*.c src/gpu/*.c src/sys/*.c)
OBJ  = $(patsubst src/%.c,build/%.o,$(SRC)) build/phawx.res.o
OUT  = build/PhawxON.exe

# plugins/<name>/<name>.c (+ <name>.rc) -> build/plugins/<name>/<name>.dll
PLUGIN_NAMES = ryzenadj example
PLUGINS      = $(foreach p,$(PLUGIN_NAMES),build/plugins/$(p)/$(p).dll)
PCFLAGS  = -Os -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type \
           -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -Iplugins/sdk
PLDFLAGS = -shared -s -static-libgcc -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va

all: $(OUT) $(PLUGINS)

$(OUT): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)
	@ls -l $@

build/%.o: src/%.c src/phawx.h src/version.h plugins/sdk/phawx_plugin.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/phawx.res.o: res/phawx.rc res/phawx.ico res/phawx.manifest src/version.h $(wildcard res/pawnio/*.bin)
	@mkdir -p build
	$(WINDRES) -I res -I src -O coff -o $@ res/phawx.rc

build/plugins/%.res.o: plugins/%.rc
	@mkdir -p $(dir $@)
	$(WINDRES) -O coff -o $@ $<

build/plugins/%.dll: plugins/%.c build/plugins/%.res.o plugins/sdk/phawx_plugin.h
	$(CC) $(PCFLAGS) $(PLDFLAGS) -o $@ $< build/plugins/$*.res.o

plugins: $(PLUGINS)

# the zip ships the RyzenAdj plugin (off until turned on) and the SDK, with the
# plugin guide as sdk/PLUGINS.md
dist: $(OUT) $(PLUGINS)
	@grep -q 'assemblyIdentity.*version="$(VERSION).0"' res/phawx.manifest || \
		{ echo "res/phawx.manifest does not carry version $(VERSION).0 from src/version.h" >&2; exit 1; }
	@rm -rf dist/PhawxON
	@mkdir -p dist/PhawxON/plugins/ryzenadj dist/PhawxON/sdk
	cp $(OUT) README.md LICENSE dist/PhawxON/
	cp res/pawnio/COPYING dist/PhawxON/LICENSE-PawnIO-Modules.txt
	cp build/plugins/ryzenadj/ryzenadj.dll dist/PhawxON/plugins/ryzenadj/
	cp plugins/sdk/phawx_plugin.h plugins/example/example.c plugins/example/example.rc docs/PLUGINS.md dist/PhawxON/sdk/
	cd dist && rm -f PhawxON-$(VERSION).zip && zip -qr PhawxON-$(VERSION).zip PhawxON

version:
	@echo $(VERSION)

clean:
	rm -rf build dist

.PHONY: all plugins dist version clean
