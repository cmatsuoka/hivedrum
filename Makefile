# Makefile for the Hive-V library (h5).
#
# Targets:
#   all     build libhive.a
#   check   build and run the test harness
#   example build the OpenAL keys player
#   clean   remove the build products

CC      ?= cc
AR      ?= ar
RANLIB  ?= ranlib
OPT     ?= -Os
HOT_OPT ?= -O2

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

LIB_CFLAGS  := $(CSTD) $(WARN) $(OPT) $(FLOAT_WARN) $(NO_UNWIND) $(NO_STACK_PROTECTOR) $(INCLUDES) $(CFLAGS) -MMD -MP
TEST_CFLAGS := $(CSTD) $(WARN) $(OPT) $(NO_UNWIND) $(INCLUDES) $(CFLAGS) -MMD -MP

BUILD := build

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
	src/h5_filter.c \
	src/h5_fixed.c \
	src/h5_noise.c \
	src/h5_mixer.c \
	src/h5_tables.c
HOT_OBJS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(HOT_SRCS))

$(HOT_OBJS): EXTRA_OPT := $(HOT_OPT)

EXAMPLES  := $(wildcard examples/*.c)
EXAMPLE_OBJS := $(patsubst examples/%.c,$(BUILD)/examples/%.o,$(EXAMPLES))

LIB  := libhive.a
TEST := h5_test
EXAMPLE := $(BUILD)/examples/play_keys

OPENAL_CFLAGS := $(shell pkg-config --cflags openal 2>/dev/null)
OPENAL_LIBS   := $(shell pkg-config --libs openal 2>/dev/null || echo -lopenal)


.PHONY: all check example play_keys clean

all: $(LIB)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(TEST): $(TEST_OBJS) $(LIB)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_OBJS) -L. -lhive -lm

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
	$(CC) $(TEST_CFLAGS) -o $@ $< -L. -lhive $(OPENAL_LIBS)

$(BUILD)/examples/play_keys.o: examples/play_keys.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) $(OPENAL_CFLAGS) -c -o $@ $<

check: $(TEST) $(EXAMPLE_OBJS)
	./$(TEST) --init
	./$(TEST) --sine
	./$(TEST) --dsp
	./$(TEST) --silence $(BUILD)/silence.wav
	./$(TEST) --api
	./$(TEST) --kick
	./$(TEST) --tom
	./$(TEST) --snare
	./$(TEST) --mixer
	./$(TEST) --velocity

clean:
	rm -rf $(BUILD) $(LIB) $(TEST)

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(EXAMPLE_OBJS:.o=.d)
