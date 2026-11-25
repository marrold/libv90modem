# v42/ build: ITU-T V.42 (LAPM) error control, V.42bis and V.44 compression.
# Portable C99, no floating point, no dynamic allocation, no OS calls.
#
#   make test          host unit and end-to-end tests
#   make test-arm      same tests cross-built (-static) and run under qemu-arm
#   make test-interop  host only: our side against spandsp as independent peer

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
LDLIBS  ?= -lm
ROOT    := $(abspath $(CURDIR)/..)
BUILD   := build
INC     := -I. -I$(ROOT)/test

ARM_CC  ?= $(wildcard $(ROOT)/toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

SRC     := v42_hdlc.c v42_detect.c v42_comp.c v44_comp.c v42_lapm.c v42.c
HDR     := v42_hdlc.h v42_detect.h v42_comp.h v44_comp.h comp_out.h v42_lapm.h v42.h
TESTS   := test_hdlc test_detect test_comp test_v44 test_lapm test_v42 test_v42_golden test_comp_out test_fcs

.PHONY: all test test-arm test-interop clean build-tests build-tests-arm print-tests print-tests-arm
all: test

$(BUILD):
	@mkdir -p $(BUILD)

# the sources compiled once, into an archive each test links: $(BUILD)/libtest.a,
# and for ARM_CC $(BUILD)/arm/libtest.a (../x.c compiles to up/x.o)
EXTRA_CPPFLAGS ?=
T_SRC     := $(SRC)
T_HDR     := $(HDR) $(ROOT)/test/test.h
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
	$(CC) $(T_FLAGS) -o $@ $< $(BUILD)/libtest.a $(LDLIBS)

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running v42 host tests =="
	@fail=0; for t in $(TESTS); do $(abspath $(BUILD))/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

$(BUILD)/arm_test_%: test/test_%.c $(BUILD)/arm/libtest.a $(T_HDR) | $(BUILD)
	$(ARM_CC) $(T_FLAGS) -static -o $@ $< $(BUILD)/arm/libtest.a $(LDLIBS)

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running v42 ARM tests under $(QEMU) =="
	@if $(CURDIR)/../tools/par-run.sh $(abspath $(BUILD))/arm_log "$(QEMU)" "[arm]" $(addprefix $(abspath $(BUILD))/arm_,$(TESTS)); \
	 then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

# interop against spandsp (host only; the peer is never shipped). All spandsp
# includes live in test/peer_spandsp_v42.c, which exposes a plain C API.
SPANDSP := $(ROOT)/peers/spandsp
INTEROP := interop_v42_spandsp interop_v42bis_spandsp
$(BUILD)/interop_%: test/interop_%.c test/peer_spandsp_v42.c test/peer_spandsp_v42.h $(SRC) $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-parameter $(INC) -I$(SPANDSP)/include -o $@ $< test/peer_spandsp_v42.c $(SRC) \
	  -L$(SPANDSP)/lib -Wl,-rpath,$(SPANDSP)/lib -lspandsp -ltiff $(LDLIBS)

test-interop: $(addprefix $(BUILD)/,$(INTEROP))
	@echo "== running v42 interop tests =="
	@fail=0; for t in $(INTEROP); do $(abspath $(BUILD))/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL INTEROP TESTS PASS"; else echo "INTEROP TESTS FAILED"; exit 1; fi

# the test programs built, not run; their paths (the top level's one pool)
build-tests: $(addprefix $(BUILD)/,$(TESTS))
build-tests-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
print-tests:
	@for t in $(TESTS); do echo $(abspath $(BUILD))/$$t; done
print-tests-arm:
	@for t in $(TESTS); do echo $(abspath $(BUILD))/arm_$$t; done

clean:
	rm -rf $(BUILD)
