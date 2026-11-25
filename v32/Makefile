# v90d/v32 - V.32bis answering and calling modem: host tests and ARM cross tests under qemu.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
BUILD   := build

# Cross toolchain for the ARM926 target (same one the top-level Makefile uses).
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

INC     := -I. -I../test -I../sim -I../modem -I../v34
# with the sine table (v32_rx) and the V.34 scrambler (v32_code) of the tree
LIB_SRC := v32_tabs.c v32_code.c v32_viterbi.c v32_tx.c v32_ec.c v32_rx.c v32.c v32_call.c ../modem/sintab.c
LIB_HDR := v32.h v32_priv.h ../modem/sintab.h ../v34/v34.h
# test side (double allowed): the calling peer, the line, and what they need
SIM_SRC := ../sim/v32call.c ../sim/v32line.c ../sim/g711.c ../sim/noise.c ../sim/filt.c \
           ../sim/resamp.c ../sim/channel.c
SIM_HDR := ../sim/v32call.h ../sim/v32line.h ../sim/v32spec.h ../test/test.h test/calltest.h
# V.8 hand-over test: the project's own V.8 answerer and calling V.8 peer
V8_SRC  := ../modem/v21.c ../modem/ansam.c ../modem/v8msg.c ../modem/async.c ../modem/v8rx.c \
           ../modem/v8ans.c ../sim/v8call.c
TESTS   := test_code test_viterbi test_tx test_ec test_rx test_startup test_call test_v8 test_acdet test_caller

.PHONY: all test test-arm cost-arm tables clean build-tests build-tests-arm print-tests print-tests-arm
all: test

$(BUILD):
	@mkdir -p $(BUILD)

# the sources compiled once, into an archive each test links: $(BUILD)/libtest.a,
# and for ARM_CC $(BUILD)/arm/libtest.a (../x.c compiles to up/x.o)
EXTRA_CPPFLAGS ?=
T_SRC     := $(LIB_SRC) $(SIM_SRC) $(V8_SRC)
T_HDR     := $(LIB_HDR) $(SIM_HDR)
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
# (the end-to-end tests shorten their runs when built for ARM)
$(BUILD)/arm_test_%: test/test_%.c $(BUILD)/arm/libtest.a $(T_HDR) | $(BUILD)
	$(ARM_CC) $(T_FLAGS) -static -o $@ $< $(BUILD)/arm/libtest.a -lm

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@if $(CURDIR)/../tools/par-run.sh $(abspath $(BUILD))/arm_log "$(QEMU)" "[arm]" $(addprefix $(abspath $(BUILD))/arm_,$(TESTS)); \
	 then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

# the modem alone must not use floating point or the allocator: no such
# symbol may be referenced by any of its objects (ARM is soft-float, so any
# float arithmetic shows up as a call into libgcc)
.PHONY: check-fixed
check-fixed: | $(BUILD)
	@for f in $(LIB_SRC); do $(ARM_CC) $(CFLAGS) $(INC) -c $$f -o $(BUILD)/chk.o || exit 1; \
	   if $(dir $(ARM_CC))arm-linux-nm $(BUILD)/chk.o | grep -E ' U (__aeabi_[df]|__aeabi_u?[il]2[df]|__(add|sub|mul|div|fix|float|extend|trunc)[a-z]*[sd]f|malloc|calloc|realloc|free|sin|cos|sqrt|exp|log|pow)'; \
	   then echo "$$f: floating point or allocation"; exit 1; fi; done; echo "no floating point, no allocation in $(LIB_SRC)"

# Exact ARM instructions per sample (see test/test_bench.c): the host records
# what the answerer receives over a whole call; qemu replays the first n
# samples into the ARM build of the modem alone, logging one line per executed
# instruction. The cost of a stretch is the difference between two values of
# n inside it. Slow (minutes), so not part of test-arm.
$(BUILD)/test_bench: test/test_bench.c $(LIB_SRC) $(LIB_HDR) $(SIM_SRC) $(SIM_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIB_SRC) $(SIM_SRC) -lm
$(BUILD)/arm_test_bench: test/test_bench.c $(LIB_SRC) $(LIB_HDR) | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -static -o $@ $< $(LIB_SRC)

COUNT    = $(QEMU) -one-insn-per-tb -d exec,nochain $(abspath $(BUILD))/arm_test_bench
NINSN    = 2>&1 | grep -c '^Trace'
BOX_KMAC ?= 25400
cost-arm: $(BUILD)/test_bench $(BUILD)/arm_test_bench
	@echo "== ARM instructions per sample (qemu -one-insn-per-tb), 8000 samples/s =="
	@f1=$$($(COUNT) fir 256 $(NINSN)); f2=$$($(COUNT) fir 768 $(NINSN)); \
	 mac100=$$(( (f2 - f1) * 100 / (512 * 128) )); \
	 echo "  reference FIR: $$(( mac100 / 100 )).$$(printf %02d $$(( mac100 % 100 ))) instructions per MAC"; \
	 for rate in 4 0; do \
	  eval $$($(abspath $(BUILD))/test_bench record $$rate $(BUILD)/bench$$rate.raw) || exit 1; \
	  $(QEMU) $(abspath $(BUILD))/arm_test_bench check $(BUILD)/bench$$rate.raw || exit 1; \
	  for w in "TRN1 (echo canceller training):$$TRN:300:1000" "receiver training (caller's TRN):$$RXT:300:1000" "data, both ways:$$DATA:2000:4000"; do \
	   name=$${w%%:*}; r=$${w#*:}; base=$${r%%:*}; r=$${r#*:}; off=$${r%%:*}; len=$${r#*:}; \
	   if [ $$rate -eq 0 ] && [ "$$name" != "data, both ways" ]; then continue; fi; \
	   a=$$($(COUNT) count $(BUILD)/bench$$rate.raw $$(( base + off )) $(NINSN)); \
	   b=$$($(COUNT) count $(BUILD)/bench$$rate.raw $$(( base + off + len )) $(NINSN)); \
	   per=$$(( (b - a) / len )); macs=$$(( per * 100 / mac100 )); share=$$(( macs * 8000 / $(BOX_KMAC) )); \
	   echo "  rate $$rate, $$name: $$per instructions per sample = $$macs MACs," \
	        "about $$(( share / 10 )).$$(( share % 10 )) % of the box"; \
	  done; done

tables:
	python3 tools/gen_tabs.py

# the test programs built, not run; their paths (the top level's one pool)
build-tests: $(addprefix $(BUILD)/,$(TESTS))
build-tests-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
print-tests:
	@for t in $(TESTS); do echo $(abspath $(BUILD))/$$t; done
print-tests-arm:
	@for t in $(TESTS); do echo $(abspath $(BUILD))/arm_$$t; done

clean:
	rm -rf $(BUILD)
