# PS5 injector payload (run inside WSL). Needs ps5-payload-sdk.
PS5_PAYLOAD_SDK ?= $(HOME)/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF      := ../../build/ldinject.elf
BLOB     := ../../build/ldhook.bin
OBJ      := ../../build/obj
OVL      := ../overlay
CFLAGS   := -Wall -O2 -I../core -DLD_INJECTOR=1
OVLFLAGS := -O2 -Wall -Wno-unused-function -I$(OVL)/common -I$(OVL)/controller -I$(OVL)/shsrv
CXXFLAGS := -std=c++20 -fno-exceptions $(OVLFLAGS)

C_SRC   := main.c pt.c webremote.c cui.c ../core/ldproto.c ovl_renderer.c
OVL_C   := $(OVL)/controller/ovl_pt.c $(OVL)/controller/stable_elfldr_bridge.c
OVL_CXX := $(OVL)/controller/ovl_controller.cpp $(OVL)/controller/remote_call.cpp \
           $(OVL)/controller/shellui_hook_controller.cpp $(OVL)/controller/stable_shellui_injector.cpp
OBJS    := $(addprefix $(OBJ)/,$(notdir $(C_SRC:.c=.o) $(OVL_C:.c=.o) $(OVL_CXX:.cpp=.o)))

vpath %.c . ../core $(OVL)/controller
vpath %.cpp $(OVL)/controller

all: $(ELF)

blob.h: $(BLOB)
	cd ../../build && xxd -i -n LDHOOK_BIN ldhook.bin > ../src/injector/blob.h

ovl_renderer.c:
	$(MAKE) -C $(OVL)

$(OBJ)/main.o: main.c blob.h offsets.h ../hook/ldhook.h cui.h webremote.h
$(OBJ)/webremote.o: webremote.c webremote.h vtp_api.h ../hook/ldhook.h
$(OBJ)/cui.o: cui.c cui.h vtp_api.h $(OVL)/common/ovl_proto.h

$(OBJ)/%.o: %.c
	@mkdir -p $(OBJ)
	$(CC) $(CFLAGS) $(if $(filter $(OVL)/%,$<),$(OVLFLAGS)) -c -o $@ $<

$(OBJ)/ovl_pt.o $(OBJ)/stable_elfldr_bridge.o: CFLAGS += $(OVLFLAGS)

$(OBJ)/%.o: %.cpp
	@mkdir -p $(OBJ)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(ELF): $(OBJS)
	$(CXX) -o $@ $(OBJS) -lkernel_sys -lpthread
	@size=$$(stat -c %s $@); echo "ldinject.elf: $$size bytes"; \
	  if [ $$size -gt 2097152 ]; then echo "warning: payload is over 2 MiB" >&2; fi

clean:
	rm -rf $(ELF) blob.h $(OBJ)
