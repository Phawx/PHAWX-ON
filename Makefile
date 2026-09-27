CC      ?= x86_64-w64-mingw32-gcc
WINDRES ?= x86_64-w64-mingw32-windres
ifeq ($(CC),cc)
CC = x86_64-w64-mingw32-gcc
endif

CFLAGS  = -Os -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -Wno-cast-function-type -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables \
          -fno-stack-protector -flto=auto -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Isrc
LDFLAGS = -s -flto=auto -mwindows -municode -Wl,--gc-sections -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va
LIBS    = -luser32 -lgdi32 -lshell32 -ladvapi32 -lpowrprof -lhid -lsetupapi -ldwmapi -lole32 -lmsimg32 -lsetupapi

SRC  = $(wildcard src/*.c src/hw/*.c src/cpu/*.c src/gpu/*.c src/sys/*.c)
OBJ  = $(patsubst src/%.c,build/%.o,$(SRC)) build/phawx.res.o
OUT  = build/PhawxON.exe

all: $(OUT)

$(OUT): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)
	@ls -l $@

build/%.o: src/%.c src/phawx.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/phawx.res.o: res/phawx.rc res/phawx.ico res/phawx.manifest
	@mkdir -p build
	$(WINDRES) -I res -O coff -o $@ res/phawx.rc

dist: $(OUT)
	@mkdir -p dist/PhawxON
	cp $(OUT) README.md LICENSE dist/PhawxON/
	cd dist && rm -f PhawxON-1.0.0.zip && zip -qr PhawxON-1.0.0.zip PhawxON

clean:
	rm -rf build dist

.PHONY: all dist clean
