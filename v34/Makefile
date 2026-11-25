# v90d/v34 - V.34 primary-channel encoder / decoder library (bits <-> 2D
# points): host tests and ARM cross tests under qemu.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror
BUILD   := build

# Cross toolchain for the ARM926 target (same one the top-level Makefile uses).
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

# V34_TEST_API: the diagnostics only the tests use (v34_rx_set_hard, v34_rx_flush)
INC     := -I. -I../test -DV34_TEST_API
LIB_SRC := v34_constel.c v34_core.c v34_params.c v34_shell.c v34_train.c \
           v34_tx.c v34_rx.c v34_nl.c
LIB_HDR := v34.h v34_priv.h
SUP_HDR := ../test/test.h test/fig5_data.h test/spec_tables.h test/testutil.h \
           ../modem/compiler.h ../modem/fixdiv.h ../modem/divrcp.h
TESTS   := test_scr test_params test_constel test_shell test_trellis \
           test_train test_roundtrip test_precoder test_ber test_resync \
           test_bench test_nl test_sflush test_nl_exact test_nl_exact_nb \
           test_vit_hash

.PHONY: all test test-arm cost-arm tables clean build-tests build-tests-arm print-tests print-tests-arm
all: test

$(BUILD):
	@mkdir -p $(BUILD)

# the sources compiled once, into an archive each test links: $(BUILD)/libtest.a,
# and for ARM_CC $(BUILD)/arm/libtest.a (../x.c compiles to up/x.o)
EXTRA_CPPFLAGS ?=
T_SRC     := $(LIB_SRC)
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

$(BUILD)/test_%: test/test_%.c $(BUILD)/libtest.a $(T_HDR) | $(BUILD)
	$(CC) $(T_FLAGS) -o $@ $< $(BUILD)/libtest.a -lm

# test_nl_exact once more with every builtin's C fallback (V90M_NO_BUILTINS):
# this file and the library's v34_nl.c (linked ahead of libtest.a's copy)
NB_FLAGS = $(T_FLAGS) -DV90M_NO_BUILTINS
$(BUILD)/test_nl_exact_nb: test/test_nl_exact.c v34_nl.c $(BUILD)/libtest.a $(T_HDR) | $(BUILD)
	$(CC) $(NB_FLAGS) -o $@ test/test_nl_exact.c v34_nl.c $(BUILD)/libtest.a -lm

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do $(abspath $(BUILD))/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

# cross-build the same tests for the ARM926 target and run under qemu
# (the tests shorten their own Monte Carlo runs when built for ARM)
$(BUILD)/arm_test_%: test/test_%.c $(BUILD)/arm/libtest.a $(T_HDR) | $(BUILD)
	$(ARM_CC) $(T_FLAGS) -static -o $@ $< $(BUILD)/arm/libtest.a -lm

$(BUILD)/arm_test_nl_exact_nb: test/test_nl_exact.c v34_nl.c $(BUILD)/arm/libtest.a $(T_HDR) | $(BUILD)
	$(ARM_CC) $(NB_FLAGS) -static -o $@ test/test_nl_exact.c v34_nl.c $(BUILD)/arm/libtest.a -lm

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@if $(CURDIR)/../tools/par-run.sh $(abspath $(BUILD))/arm_log "$(QEMU)" "[arm]" $(addprefix $(abspath $(BUILD))/arm_,$(TESTS)); \
	 then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

# Exact ARM instruction counts per 2D symbol: qemu logs one line per executed
# instruction; decoder = (encoder + decoder) - (encoder), between two run
# lengths so that start-up cancels. The 128-tap FIR of stage0/bench.c is
# counted the same way and gives the cost in "rfir MACs", the kernel that
# was timed on the real box (25.4 MMAC/s, in units of 1000 MAC/s below).
# Fails if the 16-state decoder exceeds COST_LIMIT instructions per symbol.
# Slow (several minutes), so not part of test-arm.
COUNT      = $(QEMU) -one-insn-per-tb -d exec,nochain $(abspath $(BUILD))/arm_test_bench count
NINSN      = 2>&1 | grep -c '^Trace'
BOX_KMAC   ?= 25400
COST_LIMIT ?= 3000
cost-arm: $(BUILD)/arm_test_bench
	@echo "== ARM instructions per 2D symbol (qemu -one-insn-per-tb) =="
	@r1=$$($(COUNT) 0 3 256 $(NINSN)); r2=$$($(COUNT) 0 3 768 $(NINSN)); \
	 mac100=$$(( (r2 - r1) * 100 / (512 * 128) )); fail=0; \
	 echo "  reference FIR: $$(( mac100 / 100 )).$$(printf %02d $$(( mac100 % 100 ))) instructions per MAC"; \
	 i=0; for name in "3200/4800  16 st" "3200/19200 16 st" "3200/31200 16 st" \
	    "3200/19200 16 st precoded" "3200/19200 32 st" "3200/19200 64 st"; do \
	  a1=$$($(COUNT) $$i 0 1024 $(NINSN)); a2=$$($(COUNT) $$i 0 3072 $(NINSN)); \
	  b1=$$($(COUNT) $$i 1 1024 $(NINSN)); b2=$$($(COUNT) $$i 1 3072 $(NINSN)); \
	  c1=$$($(COUNT) $$i 2 1024 $(NINSN)); c2=$$($(COUNT) $$i 2 3072 $(NINSN)); \
	  enc=$$(( (a2 - a1) / 2048 )); dec=$$(( (b2 - b1 - a2 + a1) / 2048 )); \
	  tent=$$(( (c2 - c1 - a2 + a1) / 2048 )); macs=$$(( dec * 100 / mac100 )); \
	  share=$$(( macs * 3200 / $(BOX_KMAC) )); \
	  echo "  $$name: encoder $$enc, decoder $$dec, with tentative decisions $$tent;" \
	       "decoder = $$macs MACs, about $$(( share / 10 )).$$(( share % 10 )) % of the box at 3200 sym/s"; \
	  if [ $$i -le 3 ] && [ $$dec -gt $(COST_LIMIT) ]; then echo "  OVER THE LIMIT of $(COST_LIMIT)"; fail=1; fi; \
	  i=$$((i + 1)); done; \
	 if [ $$fail -eq 0 ]; then echo "COST WITHIN LIMIT ($(COST_LIMIT) instructions per symbol, 16 states)"; else exit 1; fi

# regenerate v34_constel.c and test/fig5_data.h (needs python3; the outputs
# are checked in, so a normal build does not need this)
tables:
	python3 tools/gen_tables.py

# the test programs built, not run; their paths (the top level's one pool)
build-tests: $(addprefix $(BUILD)/,$(TESTS))
build-tests-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
print-tests:
	@for t in $(TESTS); do echo $(abspath $(BUILD))/$$t; done
print-tests-arm:
	@for t in $(TESTS); do echo $(abspath $(BUILD))/arm_$$t; done

clean:
	rm -rf $(BUILD)
