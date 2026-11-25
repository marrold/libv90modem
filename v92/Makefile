# v90d/v92 - V.92 PCM upstream building blocks (upstream encoder model,
# digital modem decoder, sequence codecs): host tests and ARM cross tests
# under qemu.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror
BUILD   := build

# Cross toolchain for the ARM926 target (same one the top-level Makefile uses).
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

INC     := -I. -Itest -I../test -I../v90 -I../v34 -I../modem
LIB_SRC := v92_map.c v92_mod.c v92_enc.c v92_dec.c v92_seq.c v92_info.c v92_sig.c v92_rx.c v92_design.c v92_p34d.c
LIB_HDR := v92.h v92_mod.h v92_enc.h v92_dec.h v92_seq.h v92_info.h v92_sig.h v92_rx.h v92_design.h v92_p34d.h
# pieces of the tree this library calls: the V.34 scrambler (v90_scr), the
# CRC and the V.90 DIL descriptor types (v90_seq), Figure 9 / Table 13 and the
# convolutional encoders of V.34 (v34_core, which needs v34_constel to link),
# the sine table (tonedet)
EXT_SRC := ../v90/v90_scr.c ../v90/v90_seq.c ../v34/v34_core.c ../v34/v34_constel.c \
           ../v90/v90_tx.c ../v90/v90_enc.c ../modem/ec.c ../modem/ecdly.c ../modem/tonedet.c ../modem/sintab.c
EXT_HDR := ../v90/v90_scr.h ../v90/v90_seq.h ../v34/v34.h ../v34/v34_priv.h \
           ../v90/v90_tx.h ../v90/v90_enc.h ../modem/ec.h ../modem/ecdly.h ../modem/tonedet.h ../modem/sintab.h
# test-only support: Table 1/V.90 for realistic constellations
SUP_SRC := ../v90/v90_pcm.c ../v90/v90_pcm_tab.c ../modem/pcmenc.c
SUP_HDR := ../v90/v90_pcm.h ../modem/pcmenc.h ../test/test.h test/testutil.h
TESTS   := test_mod test_map test_enc test_dec test_seq test_info test_sig test_e2e test_rx test_design

.PHONY: all test test-arm cost-arm clean build-tests build-tests-arm print-tests print-tests-arm
all: test

$(BUILD):
	@mkdir -p $(BUILD)

ALL_SRC := $(LIB_SRC) $(EXT_SRC) $(SUP_SRC)
ALL_HDR := $(LIB_HDR) $(EXT_HDR) $(SUP_HDR)

# the sources compiled once, into an archive each test links: $(BUILD)/libtest.a,
# and for ARM_CC $(BUILD)/arm/libtest.a (../x.c compiles to up/x.o)
EXTRA_CPPFLAGS ?=
T_SRC     := $(ALL_SRC)
T_HDR     := $(ALL_HDR)
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

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do $(abspath $(BUILD))/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

# cross-build the same tests for the ARM926 target and run under qemu
# (test_e2e shortens its Monte Carlo runs when built for ARM)
$(BUILD)/arm_test_%: test/test_%.c $(BUILD)/arm/libtest.a $(T_HDR) | $(BUILD)
	$(ARM_CC) $(T_FLAGS) -static -o $@ $< $(BUILD)/arm/libtest.a -lm

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@if $(CURDIR)/../tools/par-run.sh $(abspath $(BUILD))/arm_log "$(QEMU)" "[arm]" $(addprefix $(abspath $(BUILD))/arm_,$(TESTS)); \
	 then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

# Exact ARM instruction counts per upstream symbol for the decoder: qemu logs
# one line per executed instruction; the count is the difference between two
# run lengths (start-up cancels) of "encoder + decoder" minus "encoder only".
# Slow (a minute or two), so not part of test-arm.
COUNT = $(QEMU) -one-insn-per-tb -d exec,nochain $(abspath $(BUILD))/arm_test_e2e count
NINSN = 2>&1 | grep -c '^Trace'
cost-arm: $(BUILD)/arm_test_e2e
	@echo "== ARM instructions per upstream symbol (qemu -one-insn-per-tb) =="
	@for t in 0 1 2; do \
	  a1=$$($(COUNT) $$t 0 1200 $(NINSN)); a2=$$($(COUNT) $$t 0 3600 $(NINSN)); \
	  b1=$$($(COUNT) $$t 1 1200 $(NINSN)); b2=$$($(COUNT) $$t 1 3600 $(NINSN)); \
	  enc=$$(( (a2 - a1) / 2400 )); dec=$$(( (b2 - b1 - a2 + a1) / 2400 )); \
	  echo "  trellis $$t: encoder model $$enc, decoder $$dec instructions per symbol"; \
	 done

# the test programs built, not run; their paths (the top level's one pool)
build-tests: $(addprefix $(BUILD)/,$(TESTS))
build-tests-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
print-tests:
	@for t in $(TESTS); do echo $(abspath $(BUILD))/$$t; done
print-tests-arm:
	@for t in $(TESTS); do echo $(abspath $(BUILD))/arm_$$t; done

clean:
	rm -rf $(BUILD)
