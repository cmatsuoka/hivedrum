# Makefile for the Hive-V library (h5).
#
# Targets:
#   all     build libhive.a
#   check   build and run the test harness
#   example build the OpenAL keys player
#   clean   remove the build products
#
# Options:
#   The Hi-hat and Cymbal engines are compiled in by default, gated
#   behind LIBHIVE_ENGINE_HIHAT and LIBHIVE_ENGINE_CYMBAL (both passed via
#   -D in ENGINE_DEFS below). Override ENGINE_DEFS to trim the build:
#   `make ENGINE_DEFS=-DLIBHIVE_ENGINE_HIHAT` builds without the Cymbal,
#   `make ENGINE_DEFS=` builds neither; the disabled voices stay in the
#   API but render silence and accept no parameters. The same definitions
#   are passed to the test harness, which then skips the stages of the
#   engines that are not compiled in. LIBHIVE_XTRA_PRECISION
#   (the Snare's second drive harmonic, below masking) is never defined by
#   the Makefile; add -DLIBHIVE_XTRA_PRECISION to CFLAGS by hand to enable it.

CC      ?= cc
AR      ?= ar
RANLIB  ?= ranlib
OPT     ?= -Os
HOT_OPT ?= -O2

BUILD   ?= build
LIB     ?= libhive.a
TEST    ?= h5_test

ENGINE_DEFS := -DLIBHIVE_ENGINE_HIHAT -DLIBHIVE_ENGINE_CYMBAL

CSTD     := -std=c99
WARN     := -Wall -Wextra -Werror
INCLUDES := -Iinclude -Isrc
CFLAGS   ?=

# The library must not use floating point anywhere. Ask the compiler to prove
# it where the warnings are available.
FLOAT_WARN := $(shell $(CC) -Wfloat-conversion -Wdouble-promotion -Werror \
	-E -x c /dev/null >/dev/null 2>&1 && echo "-Wfloat-conversion -Wdouble-promotion")

# The library is plain C with no exceptions or backtraces. Drop the unwind
# tables unless the compiler does not know the flags.
NO_UNWIND := $(shell $(CC) -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Werror -E -x c /dev/null >/dev/null 2>&1 && \
	echo "-fno-asynchronous-unwind-tables -fno-unwind-tables")

# The library keeps no stack arrays that need runtime protection, and the
# per-frame canary costs bytes in every frame. The test harness and the
# example are host programs and keep the default protection.
NO_STACK_PROTECTOR := $(shell $(CC) -fno-stack-protector -Werror -E -x c \
	/dev/null >/dev/null 2>&1 && echo "-fno-stack-protector")

LIB_CFLAGS  := $(CSTD) $(WARN) $(OPT) $(FLOAT_WARN) $(NO_UNWIND) $(NO_STACK_PROTECTOR) $(INCLUDES) $(CFLAGS) $(ENGINE_DEFS) -MMD -MP
TEST_CFLAGS := $(CSTD) $(WARN) $(OPT) $(NO_UNWIND) $(INCLUDES) $(CFLAGS) $(ENGINE_DEFS) -MMD -MP

LIB_SRCS  := $(wildcard src/*.c)
TEST_SRCS := $(wildcard test/*.c)
LIB_OBJS  := $(patsubst src/%.c,$(BUILD)/src/%.o,$(LIB_SRCS))
TEST_OBJS := $(patsubst test/%.c,$(BUILD)/test/%.o,$(TEST_SRCS))

# Size-first default, but keep the per-sample DSP translation units at -O2.
# The files listed here run once per sample, so their code size matters
# less than their speed. The render loop's per-frame dispatch (h5.c,
# h5_voice.c) and the parameter table are cold with respect to the sample
# rate and stay at $(OPT).
HOT_SRCS := \
	src/h5_engines.c \
	src/h5_metallic.c \
	src/h5_filter.c \
	src/h5_fixed.c \
	src/h5_fx.c \
	src/h5_noise.c \
	src/h5_mixer.c \
	src/h5_tables.c
HOT_OBJS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(HOT_SRCS))

$(HOT_OBJS): EXTRA_OPT := $(HOT_OPT)

EXAMPLES  := $(wildcard examples/*.c)
EXAMPLE_OBJS := $(patsubst examples/%.c,$(BUILD)/examples/%.o,$(EXAMPLES))

EXAMPLE := $(BUILD)/examples/play_keys

OPENAL_CFLAGS := $(shell pkg-config --cflags openal 2>/dev/null)
OPENAL_LIBS   := $(shell pkg-config --libs openal 2>/dev/null || echo -lopenal)


.PHONY: all check check-default example play_keys clean

all: $(LIB)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(TEST): $(TEST_OBJS) $(LIB)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_OBJS) $(LIB) -lm

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) $(EXTRA_OPT) -c -o $@ $<

$(BUILD)/test/%.o: test/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(BUILD)/examples/%.o: examples/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -c -o $@ $<

example: play_keys

play_keys: $(EXAMPLE)

$(EXAMPLE): $(BUILD)/examples/play_keys.o $(LIB)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LIB) $(OPENAL_LIBS)

$(BUILD)/examples/play_keys.o: examples/play_keys.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) $(OPENAL_CFLAGS) -c -o $@ $<

check: check-default

check-default: $(TEST) $(EXAMPLE_OBJS)
	./$(TEST) --init
	./$(TEST) --sine
	./$(TEST) --dsp
	./$(TEST) --silence $(BUILD)/silence.wav
	./$(TEST) --api
	./$(TEST) --kick
	./$(TEST) --tom
	./$(TEST) --snare
	./$(TEST) --hihat
	./$(TEST) --cymbal
	./$(TEST) --mixer
	./$(TEST) --velocity
	./$(TEST) --fx

clean:
	rm -rf $(BUILD) build-xtra $(LIB) $(TEST) libhive-xtra.a h5_test-xtra

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(EXAMPLE_OBJS:.o=.d)
