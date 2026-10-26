# PS5 injector payload (run inside WSL). Needs ps5-payload-sdk.
PS5_PAYLOAD_SDK ?= $(HOME)/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF    := ../../build/ldinject.elf
BLOB   := ../../build/ldhook.bin
CFLAGS := -Wall -O2 -I../core -DLD_INJECTOR=1

all: $(ELF)

blob.h: $(BLOB)
	cd ../../build && xxd -i -n LDHOOK_BIN ldhook.bin > ../src/injector/blob.h

$(ELF): main.c pt.c webremote.c webremote.h ../core/ldproto.c blob.h offsets.h ../hook/ldhook.h
	$(CC) $(CFLAGS) -o $@ main.c pt.c webremote.c ../core/ldproto.c -lkernel_sys -lpthread

clean:
	rm -f $(ELF) blob.h
