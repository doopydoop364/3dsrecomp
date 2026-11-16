# Robobot native port: build the recompiled code and the differential test runner.
CC      := clang
GEN     := build/gen
OBJ     := build/obj
OPT     ?= -O1
CFLAGS  := $(OPT) -w -fwrapv -fno-strict-aliasing -ffp-contract=off -Iruntime -I$(GEN)
SRCS    := $(wildcard $(GEN)/rc_*.c) $(GEN)/table.c
OBJS    := $(patsubst $(GEN)/%.c,$(OBJ)/%.o,$(SRCS))

all: build/runner build/robobot

$(OBJ)/%.o: $(GEN)/%.c runtime/recomp.h $(GEN)/funcs.h $(GEN)/layout.h | $(OBJ)
	$(CC) $(CFLAGS) -DRECOMP_CHECK_RETURNS -c $< -o $@

$(OBJ):
	mkdir -p $@

build/runner: tests/runner.c runtime/vfp.c runtime/recomp.h $(OBJS)
	$(CC) -O1 -g -Iruntime -I$(GEN) tests/runner.c runtime/vfp.c $(OBJS) -o $@ -lz -lm

RT := runtime/kernel.c runtime/services.c runtime/gpu.c runtime/pica.c runtime/pica_shader.c runtime/video.c runtime/input.c runtime/settings.c runtime/dsp.c runtime/audio.c runtime/y2r.c runtime/nfc.c runtime/romfs.c runtime/vk.c runtime/main.c runtime/vfp.c
SPV := $(GEN)/pica_vert.inc $(GEN)/pica_frag.inc
$(GEN)/pica_%.inc: runtime/shaders/pica.%
	glslc -O --target-env=vulkan1.3 -mfmt=c $< -o $@

build/robobot: $(RT) runtime/host.h runtime/pica.h runtime/vkr.h runtime/recomp.h $(OBJS) $(SPV)
	$(CC) -O3 -g -Wall -Iruntime -I$(GEN) $(shell pkg-config --cflags sdl3) $(RT) $(OBJS) -o $@ $(shell pkg-config --libs sdl3) -lvulkan -lz -lm

build/gpu_replay: tests/gpu_replay.c runtime/pica.c runtime/pica_shader.c runtime/vfp.c runtime/vk.c runtime/pica.h runtime/vkr.h runtime/host.h $(GEN)/layout.h $(SPV)
	$(CC) -O3 -g -Wall -Iruntime -I$(GEN) tests/gpu_replay.c runtime/pica.c runtime/pica_shader.c runtime/vfp.c runtime/vk.c -o $@ -lvulkan -lm

clean:
	rm -rf $(OBJ) build/runner
.PHONY: all clean
