# Console overlay renderer, loaded into SceShellUI by the injector (see controller/).
PS5_PAYLOAD_SDK ?= $(HOME)/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

OUT      := ../../build/vtp_shellui.elf
EMBED    := ../injector/ovl_renderer.c
CXXFLAGS := --target=x86_64-sie-ps5 -std=c++20 -DPS5 -fPIC -fvisibility=hidden -march=znver2 -O2 \
            -fno-exceptions -ffunction-sections -fdata-sections -Wall -Wno-unused-function -Icommon
LDFLAGS  := -shared -Wl,--gc-sections -Wl,-Bsymbolic -Wl,-e,elf_main -Wl,-soname,libVtpOverlay.so \
            -Wl,-z,now -Wl,--hash-style=sysv
LIBS     := -lkernel_sys -lSceLibcInternal -lSceNet

all: $(EMBED)

../../build/vtp_shellui.o: renderer/vtp_shellui.cpp renderer/engine.inc renderer/vtp_shellui.hpp common/ovl_proto.h
	@mkdir -p ../../build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

../../build/vtp_shellui_entry.o: renderer/vtp_shellui_entry.cpp renderer/vtp_shellui.hpp
	@mkdir -p ../../build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT): ../../build/vtp_shellui.o ../../build/vtp_shellui_entry.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)
	$(STRIP) --strip-unneeded $@
	python3 ../../tools/check_renderer.py $@

$(EMBED): $(OUT)
	cd ../../build && xxd -i -n OVL_RENDERER_ELF vtp_shellui.elf | sed 's/^unsigned/const unsigned/' > ../src/injector/ovl_renderer.c

clean:
	rm -f $(OUT) $(EMBED) ../../build/vtp_shellui.o ../../build/vtp_shellui_entry.o
