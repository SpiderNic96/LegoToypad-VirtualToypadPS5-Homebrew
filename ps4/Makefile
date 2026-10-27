# LEGO Dimensions - Virtual Toy Pad, PS4 GoldHEN plugin (.prx)
#
# Needs the OpenOrbis PS4 Toolchain (v0.5.2+) and the GoldHEN Plugin SDK (built):
#   export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
#   export GOLDHEN_SDK=/path/to/GoldHEN_Plugins_SDK
#   make            -> build/VirtualToyPad.prx
# Same hook module and phone-remote code as the PS5 payload (src/hook, src/core,
# src/injector/webremote.c), built with -DLD_PS4_PLUGIN.

TOOLCHAIN ?= $(OO_PS4_TOOLCHAIN)
GH_SDK    ?= $(GOLDHEN_SDK)
ifeq ($(origin CC),default)
CC        := clang
endif
LD        := ld.lld
UNAME_S   := $(shell uname -s)
CDIR      := $(if $(filter Darwin,$(UNAME_S)),macos,linux)

NAME   := VirtualToyPad
OUT    := build
SRC    := ..

COMMON := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -DLD_PS4_PLUGIN \
          -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -I$(GH_SDK)/include \
          -Wall -Wno-unused-parameter -Wno-unused-function
# hook module: no libc calls of its own, everything hidden (same as the PS5 blob)
HOOKFLAGS := $(COMMON) -fno-stack-protector -fno-jump-tables -mstackrealign -I$(SRC)/src/core
# plugin side: links the system libc; LD_INJECTOR keeps ldhook.h from hiding libc declarations
APPFLAGS  := $(COMMON) -DLD_INJECTOR -I$(SRC)/src/core

LIBS    := -lSceLibcInternal -lGoldHEN_Hook -lkernel -lSceSysmodule -lScePad -lSceUsbd
LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x -e _init --eh-frame-hdr \
           -L$(TOOLCHAIN)/lib -L$(GH_SDK) $(LIBS)

OBJS := $(OUT)/main.o $(OUT)/webremote.o $(OUT)/hooks.o $(OUT)/menu.o $(OUT)/ldproto.o

all: $(OUT)/$(NAME).prx

$(OUT):
	mkdir -p $(OUT)

$(OUT)/main.o: source/main.c $(SRC)/src/hook/ldhook.h $(SRC)/src/injector/offsets.h | $(OUT)
	$(CC) $(APPFLAGS) -c $< -o $@
$(OUT)/webremote.o: $(SRC)/src/injector/webremote.c source/ps4_shim.h | $(OUT)
	$(CC) $(APPFLAGS) -c $< -o $@
$(OUT)/hooks.o: $(SRC)/src/hook/hooks.c $(SRC)/src/hook/ldhook.h | $(OUT)
	$(CC) $(HOOKFLAGS) -c $< -o $@
$(OUT)/menu.o: $(SRC)/src/hook/menu.c $(SRC)/src/hook/ldhook.h | $(OUT)
	$(CC) $(HOOKFLAGS) -c $< -o $@
$(OUT)/ldproto.o: $(SRC)/src/core/ldproto.c $(SRC)/src/core/ldproto.h | $(OUT)
	$(CC) $(HOOKFLAGS) -c $< -o $@

$(OUT)/$(NAME).prx: $(OBJS)
	$(LD) $(GH_SDK)/build/crtprx.o $(OBJS) -o $(OUT)/$(NAME).elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=$(OUT)/$(NAME).elf -out=$(OUT)/$(NAME).oelf \
	    --lib=$@ --paid 0x3800000000000011

clean:
	rm -rf $(OUT)

.PHONY: all clean
