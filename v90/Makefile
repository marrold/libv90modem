# v90d/v90 - V.90 digital modem transmit side (Phase 3 onwards) and the
# sequence codecs: host tests and ARM cross tests under qemu.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror
BUILD   := build

# Cross toolchain for the ARM926 target (same one the top-level Makefile uses).
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

INC     := -I. -Itest -I../test -I../v34
LIB_SRC := v90_pcm.c v90_pcm_tab.c v90_scr.c v90_seq.c v90_enc.c v90_tx.c
# v90_scr wraps the V.34 scrambler of ../v34/v34.h (header only)
LIB_HDR := v90_pcm.h v90_scr.h v90_seq.h v90_enc.h v90_tx.h ../v34/v34.h
# test-only support: the reference decoder (never linked into the modem)
SUP_SRC := test/v90_dec.c
SUP_HDR := test/v90_dec.h test/table1.inc ../test/test.h
TESTS   := test_pcm test_scr test_seq test_enc test_tx test_tx92 test_e2e

.PHONY: all test test-arm clean build-tests build-tests-arm print-tests print-tests-arm
all: test

$(BUILD):
	@mkdir -p $(BUILD)

# the sources compiled once, into an archive each test links: $(BUILD)/libtest.a,
# and for ARM_CC $(BUILD)/arm/libtest.a (../x.c compiles to up/x.o)
EXTRA_CPPFLAGS ?=
T_SRC     := $(LIB_SRC) $(SUP_SRC)
T_HDR     := $(LIB_HDR) $(SUP_HDR)
T_FLAGS    = $(CFLAGS) $(EXTRA_CPPFLAGS) $(INC)
T_OBJ     := $(patsubst %.c,$(BUILD)/tobj/%.o,$(subst ../,up/,$(T_SRC)))
ARM_T_OBJ := $(patsubst %.c,$(BUILD)/arm/tobj/%.o,$(subst ../,up/,$(T_SRC)))
# ar next to the compiler (arm-linux-gcc -> arm-linux-ar) when there is one
ar_for     = $(firstword $(foreach a,$(patsubst %gcc,%ar,$(filter %gcc,$(1))),$(shell command -v $(a))) ar)
$(BUILD)/tobj/%.o: %.c $(T_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(T_FLAGS) -c -o $@ $<
$(BUILD)/tobj/up/%.o: ../%.c $(T_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(T_FLAGS) -c -o $@ $<
$(BUILD)/arm/tobj/%.o: %.c $(T_HDR)
	@mkdir -p $(dir $@)
	$(ARM_CC) $(T_FLAGS) -c -o $@ $<
$(BUILD)/arm/tobj/up/%.o: ../%.c $(T_HDR)
	@mkdir -p $(dir $@)
	$(ARM_CC) $(T_FLAGS) -c -o $@ $<
$(BUILD)/libtest.a: $(T_OBJ)
	rm -f $@ && $(call ar_for,$(CC)) rcs $@ $(T_OBJ)
$(BUILD)/arm/libtest.a: $(ARM_T_OBJ)
	rm -f $@ && $(call ar_for,$(ARM_CC)) rcs $@ $(ARM_T_OBJ)

# each test_X links test/test_X.c with the library and the reference decoder
$(BUILD)/test_%: test/test_%.c $(BUILD)/libtest.a $(T_HDR) | $(BUILD)
	$(CC) $(T_FLAGS) -o $@ $< $(BUILD)/libtest.a -lm

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do $(abspath $(BUILD))/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

# cross-build the same tests for the ARM926 target and run under qemu
$(BUILD)/arm_test_%: test/test_%.c $(BUILD)/arm/libtest.a $(T_HDR) | $(BUILD)
	$(ARM_CC) $(T_FLAGS) -static -o $@ $< $(BUILD)/arm/libtest.a -lm

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@if $(CURDIR)/../tools/par-run.sh $(abspath $(BUILD))/arm_log "$(QEMU)" "[arm]" $(addprefix $(abspath $(BUILD))/arm_,$(TESTS)); \
	 then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

# the test programs built, not run; their paths (the top level's one pool)
build-tests: $(addprefix $(BUILD)/,$(TESTS))
build-tests-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
print-tests:
	@for t in $(TESTS); do echo $(abspath $(BUILD))/$$t; done
print-tests-arm:
	@for t in $(TESTS); do echo $(abspath $(BUILD))/arm_$$t; done

clean:
	rm -rf $(BUILD)
