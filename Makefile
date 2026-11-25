# libv90modem: build, test, install.
#
#   make                 static and shared library, pkg-config file, examples (in $(BUILD))
#   make check           the simulator suite on the host; with PAR=N, every program of
#                        every directory in one pool, N at a time (tools/par-run.sh)
#   make check-arm       the same, cross-built (ARM_CC) and run under qemu-arm,
#                        PAR (8) programs at a time, all directories in one pool
#   make test-times      tools/test-times.txt (the pool's order, longest first) from
#                        the run times of the last runs ($(BUILD)/test-times.txt)
#   make check-ubsan     the host suite with undefined behaviour trapped (clang)
#   make golden          make check, then every program's output against its golden copy
#                        (test/golden/out, tools/golden.sh); golden-record writes them
#   make bench-hashes    the benches' transmit and receive hashes on the host against
#                        test/golden/bench.txt (tools/bench-hashes.sh)
#   make check-fixed     the library compiled for armv5 (FIXED_CC): no soft-float helper,
#                        no allocation outside api/v90modem.o
#   make check-headers   every header compiles alone with -I. -Iinclude (its includes
#                        relative to itself); make check runs it
#   make check-diag-names  every engine error has its name in v90m_err_name, and every phase and modulation in its name helper
#                        (tools/check-diag-names.sh); make check runs it
#   make check-version   the Makefile's VERSION is the header's V90M_VERSION_*, README's
#                        "This is X" and docs/getting-started.md's .so names; make
#                        check runs it
#   make verify          per commit (tools/verify.sh): host check and golden outputs,
#                        replay proof, bench hashes, UBSan, the armv7hf short suite
#                        and an aarch64be (big-endian) one against the host short set
#                        (less tools/ports-slow.txt), split-check, check-fixed
#   make verify-phase    the release proof, hours (tools/verify-phase.sh): armv5, every
#                        port, armv7hf in full, the benches on qemu, costs to
#                        $(BUILD)/costs.txt, the CRd scan (RECS=)
#   make install         into $(DESTDIR)$(PREFIX); make uninstall takes it out
#   make examples        examples/answer_file (answering a recording), examples/call_loop
#                        (calling the library's own answerer; make check runs it)
#   make crd-scan        K56flex's CRd detector over recorded calls (RECS=<dir>): no detection
#   make bench-v90       record a simulated V.90 call, replay it on the host and
#                        under qemu, through the internals and the public API
#   make bench-x2        the same for x2 (a call of the simulated client); cost-x2 counts
#                        (X2_ARGS="INDEX [none|v42bis|v44]": rate index, V.42 on the bits)
#   make bench-k56       the same for K56flex (the simulated client at 56000); cost-k56 counts
#                        (K56_ARGS=heavy: a client ignoring our MP, x2's upstream)
#   make bench-v90a      the same for the V.90 analogue modem's downstream receiver
#   make bench-v90c      the same for the whole V.90 calling modem (orig, v90a)
#   make bench-v92c      ... and for it in V.92 (PCM upstream); cost-v92c counts
#   make prof-v90c       per tick and per function ARM instruction counts (prof-v92c, PPM=)
#   make bench-bell      the calling modem in Bell 103 and Bell 212A; cost-bell counts
#   make cost-v42        V.42's ARM instructions per endpoint per second (rig/bench_v42.c)
#   make cost-x2-v42     x2 data mode with our V.42 (none, V.42 bis, V.44; 32000 and 56000)
#   make test-interop    against spandsp (SPANDSP=its install prefix), host only
#   make test-x2-courier an emulated USR Courier calls our answerer (COURIER_EMU=courier-emu
#                        checkout, PAD=0|3|6, MODS=x2, STRICT_RENEG=1; tools/x2-peer/README.md),
#                        host only
#   make test-k56-sm56   Motorola's SM56 calls our answerer (DMODEM=D-Modem checkout, built;
#                        MODS=k56|none, RBS=<hex>, DOWN=<bps>, STRICT_RENEG=1;
#                        tools/k56-peer/README.md), host only, needs gcc -m32
#
# Cross-compiling: set CC (and BUILD to keep the objects apart), e.g.
#   make CC=arm-linux-gcc BUILD=build/arm
# ar, ld and objcopy are taken from the same prefix as CC.
#
# Signal-path code is portable C99, fixed-point. The simulator (sim/) is a
# test peer and may use double; it is never installed.

NAME     := libv90modem
VERSION  := 0.6.2
# the SONAME's version: MAJOR.MINOR while MAJOR is 0 (every 0.x changes the
# ABI), MAJOR from 1.0
SOVER    := $(shell echo '$(VERSION)' | awk -F. '{ print ($$1 == 0 ? $$1 "." $$2 : $$1) }')

CC       ?= cc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra -Werror
# -D options for the modem code (bench experiments, trace builds)
EXTRA_CPPFLAGS ?=
CPPFLAGS_ALL := -D_POSIX_C_SOURCE=200809L $(EXTRA_CPPFLAGS)
LDLIBS   ?= -lm
BUILD    ?= build

# binutils next to the compiler (arm-linux-gcc -> arm-linux-ar ...) when there
# are any; musl-gcc, clang and cc use the host's
CROSS    := $(if $(filter %-gcc,$(notdir $(CC))),$(patsubst %gcc,%,$(CC)),)
CROSS    := $(if $(CROSS),$(if $(shell command -v $(CROSS)ld),$(CROSS)),)
AR       := $(CROSS)ar
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy
NM       := $(CROSS)nm
READELF  := $(CROSS)readelf

PREFIX   ?= /usr/local
LIBDIR   ?= $(PREFIX)/lib
INCDIR   ?= $(PREFIX)/include
BINDIR   ?= $(PREFIX)/bin
PCDIR    ?= $(LIBDIR)/pkgconfig

ARM_CC   ?= $(firstword $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc) arm-linux-gcc)
QEMU     ?= qemu-arm-static
SPANDSP  ?= $(CURDIR)/../peers/spandsp

INC      := -Iinclude -Iapi -Imodem -Iv90 -Iv92 -Iv34 -Iv32 -Iv42 -Isim

# ---- the library ----
V42_SRC  := $(addprefix v42/,v42_hdlc.c v42_detect.c v42_comp.c v44_comp.c v42_lapm.c v42.c)
CORE_SRC := $(wildcard modem/*.c) $(wildcard v90/*.c) $(wildcard v34/*.c) $(wildcard v32/*.c) $(wildcard v92/*.c) $(V42_SRC)
API_SRC  := api/v90modem.c api/v90modem_diag.c
LIB_SRC  := $(API_SRC) $(CORE_SRC)
LIB_HDR  := include/v90modem.h api/v90modem_priv.h api/v90modem_fields.h api/v90modem_diag.h $(wildcard modem/*.h v90/*.h v34/*.h v32/*.h v92/*.h v42/*.h)
LIB_OBJ  := $(patsubst %.c,$(BUILD)/obj/%.o,$(LIB_SRC))
PIC_OBJ  := $(patsubst %.c,$(BUILD)/pic/%.o,$(LIB_SRC))

STATIC   := $(BUILD)/$(NAME).a
SHARED   := $(BUILD)/$(NAME).so.$(VERSION)
PCFILE   := $(BUILD)/$(NAME).pc
# never installed: every internal symbol visible, plus the simulator; for the
# test suite, the tools here, and for programs that test against the
# simulated callers (v90d's call tests)
DEV      := $(BUILD)/$(NAME)-dev.a

# ---- the simulator and the test suite ----
SIM_SRC  := sim/g711.c sim/noise.c sim/filt.c sim/resamp.c sim/channel.c sim/v8call.c sim/callpeer.c sim/p2a.c sim/qammod.c \
            sim/p34a.c v90/test/v90_dec.c sim/p2c.c sim/v34peer.c sim/v32call.c sim/v32line.c sim/v92chan.c sim/v92a.c sim/v92call.c sim/dschan.c sim/x2call.c sim/k56call.c
SIM_OBJ  := $(patsubst %.c,$(BUILD)/obj/%.o,$(SIM_SRC))
MODEM_SRC := modem/sintab.c modem/v34_tabs.c modem/v21.c modem/ansam.c modem/v8msg.c modem/async.c modem/v8rx.c modem/v8ans.c \
             modem/scramble.c modem/v22tx.c modem/v22rx.c modem/v22bis.c modem/v14.c modem/answer.c modem/ans_legacy.c modem/ans_v90.c modem/ans_v34.c modem/ans_x2.c modem/ans_k56.c \
             modem/info.c modem/dpsk.c modem/probe.c modem/p2d.c modem/p2common.c modem/qamrx.c modem/ec.c modem/ecdly.c modem/tonedet.c modem/v34train.c modem/p34d.c modem/v34up.c modem/pcmenc.c \
             modem/v34mp.c modem/p2ans.c modem/v34mod.c modem/v34dx.c modem/v34ans.c modem/anstone.c modem/v8orig.c modem/orig.c modem/p2call.c modem/v34orig.c modem/v90arx.c modem/v90adec.c modem/v90dil.c modem/v90a.c modem/v90a_up90.c modem/v90a_up92.c modem/v90a_down.c modem/v90a_watch.c modem/pcmtx.c modem/v92aseq.c modem/belltone.c \
             modem/x2rec.c modem/x2map.c modem/x2tab.c modem/x2bank.c modem/x2tx.c modem/x2d.c modem/k56tab.c modem/k56enc.c modem/k56tx.c modem/k56d.c modem/v8bismsg.c modem/v8bis.c \
             $(wildcard v90/*.c) $(wildcard v34/*.c) $(wildcard v32/*.c) $(wildcard v92/*.c)
T_SRC    := $(SIM_SRC) $(MODEM_SRC)
HDRS     := $(wildcard modem/*.h sim/*.h test/*.h v90/*.h v34/*.h v92/*.h v90/test/v90_dec.h)
# where the tests read their data files (test/test.h), unless EXTRA_CPPFLAGS names it
TEST_DATA := $(if $(findstring TEST_DATA_DIR,$(EXTRA_CPPFLAGS)),,-DTEST_DATA_DIR="\"$(CURDIR)/test/data\"")
T_CFLAGS := $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(EXTRA_CPPFLAGS) $(TEST_DATA) -Iv90 -Imodem -Isim -Iv92 -Iv34
TESTS    := test_g711 test_noise test_filt test_resamp test_channel test_v21 test_v23 test_ansam test_v8msg test_v8rx test_v8ans \
            test_v22tx test_v22rx test_v22bis test_v14 test_answer test_info test_dpsk test_probe test_p2 test_qamrx test_ec test_ecdly test_v34train test_sres test_answer_v90 test_p34 test_pcmenc \
            test_v34mp test_p2ans test_v34mod test_v34dx test_v34call test_answer_v34 test_answer_v32 test_answer_v90_v34 \
            test_p2_v92 test_v92call test_answer_v92 test_v92real test_answer_lin \
            test_anstone test_v8orig test_orig test_orig_v32 test_p2call test_orig_v34 test_v90arx test_v90arx_robust test_v90adec test_v90dil test_p2v90 test_v90a test_orig_v90 test_orig_v90_te test_orig_seeds test_orig_retrain test_pcmtx test_v92aseq test_p2v92a test_v92a test_orig_v92 test_belltone test_orig_bell test_realcheck test_x2_pad test_answer_x2 test_x2rec test_x2map test_x2tx test_k56tab test_k56enc test_k56tx test_v8bismsg test_v8bis test_answer_k56 test_clock test_shared test_fixmath test_v34up test_ans_table test_exact test_interp_bound test_v90arx_paths test_small_exact test_v92_hostile
# the slowest programs, split by case: `prog --cases` prints the count, `prog N`
# runs case N alone, no argument all of them (tools/split-check.sh); the ARM
# runner (check-arm) runs each case as its own job
SPLIT_TESTS := test_p34 test_orig_v92 test_orig_seeds test_orig_bell
# the public API, against the library as it is installed
API_TESTS := test_api test_ec_fields test_reneg test_diag
SUBS     := v34 v90 v92 v42 v32

.PHONY: test-x2-courier test-k56-sm56 test-fixtures test-examples all lib dev install uninstall clean check check-arm check-ubsan test test-arm test-all test-times test-api test-interop examples bench-v90 bench-v34 bench-v90a bench-v90c bench-v92c replay
all: lib examples
lib: $(STATIC) $(SHARED) $(PCFILE)
dev: $(DEV)

$(BUILD)/obj/%.o: %.c $(LIB_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS_ALL) -fvisibility=hidden $(INC) -c -o $@ $<
$(BUILD)/pic/%.o: %.c $(LIB_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS_ALL) -fvisibility=hidden -fPIC $(INC) -c -o $@ $<

# Static: one relocatable object in which everything but the public API is
# renamed v90m__i_* (tools/symmap.sh), so that the internals cannot clash
# with a program's symbols. Renamed, not made local: some targets' code
# cannot reach a local symbol the way it reaches a global one.
$(STATIC): $(LIB_OBJ) tools/symmap.sh
	$(LD) -r -o $(BUILD)/$(NAME).o $(LIB_OBJ)
	tools/symmap.sh $(NM) $(BUILD)/$(NAME).o > $(BUILD)/$(NAME).map
	$(OBJCOPY) --redefine-syms=$(BUILD)/$(NAME).map $(BUILD)/$(NAME).o
	rm -f $@ && $(AR) rcs $@ $(BUILD)/$(NAME).o
# what the static library defines: the public API, its internals renamed,
# and the compiler's own helpers; nothing else
.PHONY: check-symbols
check-symbols: $(STATIC)
	@bad=$$($(NM) -g --defined-only $(STATIC) | awk 'NF >= 3 { print $$3 }' | sed 's/^_v90m_/v90m_/' \
	      | grep -vE '^(v90m_|__|\.)' || true); \
	 if [ -n "$$bad" ]; then echo "$(STATIC) defines symbols outside v90m_*:"; echo "$$bad" | head; exit 1; fi; \
	 bad=$$($(NM) -g --defined-only $(STATIC) | awk 'NF >= 3 { print $$3 }' | sed 's/^_v90m_/v90m_/' \
	      | grep -E '^v90m__' | grep -vE '^v90m__i_' || true); \
	 if [ -n "$$bad" ]; then echo "$(STATIC) defines internal v90m__* names not renamed v90m__i_*:"; echo "$$bad" | head; exit 1; fi; \
	 echo "symbols: $(STATIC) defines only v90m_* ($$($(NM) -g --defined-only $(STATIC) | grep -c ' _*v90m__i_') internal)"
# the shared library's SONAME: libv90modem.so.MAJOR.MINOR while MAJOR is 0
# (each 0.x has its own ABI), .so.MAJOR from 1.0 (DESIGN: spec D10)
.PHONY: check-soname
check-soname: $(SHARED)
	@want=$$(echo '$(VERSION)' | awk -F. '{ print ($$1 == 0 ? $$1 "." $$2 : $$1) }'); \
	 got=$$($(READELF) -d $(SHARED) | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p'); \
	 if [ "$$got" != "$(NAME).so.$$want" ]; then echo "soname: SONAME [$$got], want $(NAME).so.$$want"; exit 1; fi; \
	 echo "soname: $$got"
# one version: the Makefile's VERSION (the .so, the .pc) is the header's
# V90M_VERSION_MAJOR.MINOR.PATCH, which v90m_version() is made from; and
# the two places of the documentation that name the version in force:
# README.md's "This is X" and docs/getting-started.md's libv90modem.so.X
# (with its .so.MAJOR.MINOR SONAME). Other mentions are history.
.PHONY: check-version
check-version:
	@h=; for p in MAJOR MINOR PATCH; do \
	   n=$$(sed -n "s/^#define V90M_VERSION_$$p  *\([0-9][0-9]*\)\$$/\1/p" include/v90modem.h); \
	   [ -n "$$n" ] || { echo "version: no V90M_VERSION_$$p in include/v90modem.h"; exit 1; }; \
	   h=$$h$${h:+.}$$n; \
	 done; \
	 if [ "$$h" != "$(VERSION)" ]; then echo "version: Makefile $(VERSION), include/v90modem.h $$h"; exit 1; fi; \
	 r=$$(sed -n 's/.*This is \([0-9][0-9.]*[0-9]\);.*/\1/p' README.md); \
	 if [ "$$r" != "$(VERSION)" ]; then echo "version: Makefile $(VERSION), README.md \"This is $$r\""; exit 1; fi; \
	 g=$$(sed -n 's/.*`$(NAME)\.so\.\([0-9][0-9.]*[0-9]\)`.*/\1/p' docs/getting-started.md | sort -u | paste -sd ' ' -); \
	 if [ "$$g" != "$(SOVER) $(VERSION)" ]; then echo "version: Makefile $(VERSION) (.so.$(SOVER)), docs/getting-started.md .so.$$g"; exit 1; fi; \
	 echo "version: $(VERSION) everywhere"
# every header of the library (api/ included), its subdirectories' tests
# and include/ compiled alone with only -I. -Iinclude: each includes what it uses, by a
# path relative to itself
HDR_ALONE := $(sort $(shell find api modem v34 v90 v92 v32 v42 include -name '*.h'))
.PHONY: check-headers
check-headers:
	@mkdir -p $(BUILD)/hdr; fail=0; \
	 for h in $(HDR_ALONE); do \
	   printf '#include "%s"\n' "$$h" > $(BUILD)/hdr/alone.c; \
	   $(CC) $(CFLAGS) -I. -Iinclude -c -o $(BUILD)/hdr/alone.o $(BUILD)/hdr/alone.c || { echo "headers: $$h does not compile alone"; fail=1; }; \
	 done; \
	 [ $$fail = 0 ] && echo "headers: $(words $(HDR_ALONE)) compile alone"
# Fixed point and no allocation after create, checked on the box's compiler
# (FIXED_CC, the armv5 soft-float one): every library object compiled alone,
# its undefined symbols against an allowlist (tools/check-fixed.sh): no
# soft-float helper, no libm, malloc and free only in api/v90modem.o
FIXED_CC  ?= $(firstword $(wildcard $(CURDIR)/../toolchains/armv5-eabi--uclibc--*/bin/arm-linux-gcc))
FIXED_NM   = $(patsubst %gcc,%nm,$(FIXED_CC))
FIXED_OBJ := $(patsubst %.c,$(BUILD)/fixed/%.o,$(LIB_SRC))
$(BUILD)/fixed/%.o: %.c $(LIB_HDR)
	@mkdir -p $(dir $@)
	$(FIXED_CC) $(CFLAGS) $(CPPFLAGS_ALL) $(INC) -c -o $@ $<
.PHONY: check-fixed fixed-objs
fixed-objs: $(FIXED_OBJ)
check-fixed:
	@if [ -z "$(FIXED_CC)" ] || [ ! -x "$(FIXED_CC)" ]; then echo "check-fixed: no armv5 compiler (FIXED_CC=$(FIXED_CC))"; exit 1; fi
	@$(MAKE) -s $(if $(findstring jobserver,$(MAKEFLAGS)),,-j16) fixed-objs
	@tools/check-fixed.sh $(FIXED_NM) $(BUILD)/fixed $(FIXED_OBJ)
$(SHARED): $(PIC_OBJ)
	$(CC) -shared -Wl,-soname,$(NAME).so.$(SOVER) -o $@ $(PIC_OBJ) $(LDLIBS)
	ln -sf $(NAME).so.$(VERSION) $(BUILD)/$(NAME).so.$(SOVER)
	ln -sf $(NAME).so.$(SOVER) $(BUILD)/$(NAME).so
# the .pc file names the install paths: made again when they change
PC_VARS := $(PREFIX)|$(LIBDIR)|$(INCDIR)|$(VERSION)
.PHONY: .pc-force
$(BUILD)/pc.vars: .pc-force
	@mkdir -p $(BUILD); echo '$(PC_VARS)' | cmp -s - $@ || echo '$(PC_VARS)' > $@
$(PCFILE): $(NAME).pc.in Makefile $(BUILD)/pc.vars
	@mkdir -p $(BUILD)
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@INCDIR@|$(INCDIR)|' -e 's|@VERSION@|$(VERSION)|' $< > $@
$(DEV): $(LIB_OBJ) $(SIM_OBJ)
	rm -f $@ && $(AR) rcs $@ $(LIB_OBJ) $(SIM_OBJ)

install: lib $(BUILD)/v90m-replay
	install -d $(DESTDIR)$(INCDIR) $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PCDIR) $(DESTDIR)$(BINDIR)
	install -m 644 include/v90modem.h $(DESTDIR)$(INCDIR)/
	install -m 644 $(STATIC) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(SHARED) $(DESTDIR)$(LIBDIR)/
	ln -sf $(NAME).so.$(VERSION) $(DESTDIR)$(LIBDIR)/$(NAME).so.$(SOVER)
	ln -sf $(NAME).so.$(SOVER) $(DESTDIR)$(LIBDIR)/$(NAME).so
	install -m 644 $(PCFILE) $(DESTDIR)$(PCDIR)/
	install -m 755 $(BUILD)/v90m-replay $(DESTDIR)$(BINDIR)/
uninstall:
	rm -f $(DESTDIR)$(INCDIR)/v90modem.h $(DESTDIR)$(LIBDIR)/$(NAME).a $(DESTDIR)$(LIBDIR)/$(NAME).so \
	      $(DESTDIR)$(LIBDIR)/$(NAME).so.$(SOVER) $(DESTDIR)$(LIBDIR)/$(NAME).so.$(VERSION) \
	      $(DESTDIR)$(PCDIR)/$(NAME).pc $(DESTDIR)$(BINDIR)/v90m-replay
	@# the .so.0 link the 0.x versions up to 0.5 installed, when it now points at nothing
	@l=$(DESTDIR)$(LIBDIR)/$(NAME).so.0; if [ -L "$$l" ] && [ ! -e "$$l" ]; then rm -f "$$l"; echo "removed $$l"; fi

# ---- tests: T_SRC compiled once (T_CFLAGS) into $(BUILD)/libtest.a, and
# for ARM_CC into $(BUILD)/arm/libtest.a; each test_X links test/test_X.c
# against it ----
T_OBJ     := $(patsubst %.c,$(BUILD)/tobj/%.o,$(T_SRC))
ARM_T_OBJ := $(patsubst %.c,$(BUILD)/arm/tobj/%.o,$(T_SRC))
# ar next to ARM_CC (arm-linux-gcc -> arm-linux-ar) when there is one
ARM_AR     = $(firstword $(foreach a,$(patsubst %gcc,%ar,$(filter %gcc,$(ARM_CC))),$(shell command -v $(a))) ar)
$(BUILD)/tobj/%.o: %.c $(HDRS) $(LIB_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(T_CFLAGS) -c -o $@ $<
$(BUILD)/arm/tobj/%.o: %.c $(HDRS) $(LIB_HDR)
	@mkdir -p $(dir $@)
	$(ARM_CC) $(T_CFLAGS) -c -o $@ $<
$(BUILD)/libtest.a: $(T_OBJ)
	rm -f $@ && $(AR) rcs $@ $(T_OBJ)
$(BUILD)/arm/libtest.a: $(ARM_T_OBJ)
	rm -f $@ && $(ARM_AR) rcs $@ $(ARM_T_OBJ)
$(BUILD)/test_%: test/test_%.c $(BUILD)/libtest.a $(HDRS)
	$(CC) $(T_CFLAGS) -o $@ $< $(BUILD)/libtest.a $(LDLIBS)
$(BUILD)/arm_test_%: test/test_%.c $(BUILD)/arm/libtest.a $(HDRS)
	$(ARM_CC) $(T_CFLAGS) -static -o $@ $< $(BUILD)/arm/libtest.a $(LDLIBS)
# x2's pad octets: a host tool (tools/x2_pad.c), not in the library; its
# test links the tool and the G.711 codec alone
X2PAD_SRC := tools/x2_pad.c sim/g711.c
$(BUILD)/test_x2_pad: test/test_x2_pad.c $(X2PAD_SRC) tools/x2_pad.h sim/g711.h test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ $< $(X2PAD_SRC) $(LDLIBS)
$(BUILD)/arm_test_x2_pad: test/test_x2_pad.c $(X2PAD_SRC) tools/x2_pad.h sim/g711.h test/test.h
	@mkdir -p $(BUILD)
	$(ARM_CC) $(T_CFLAGS) -static -o $@ $< $(X2PAD_SRC) $(LDLIBS)
# the mapper's test checks modem/x2tab's pad octets against the tool's rule
$(BUILD)/test_x2map: test/test_x2map.c $(BUILD)/libtest.a tools/x2_pad.c tools/x2_pad.h $(HDRS)
	$(CC) $(T_CFLAGS) -o $@ $< tools/x2_pad.c $(BUILD)/libtest.a $(LDLIBS)
$(BUILD)/arm_test_x2map: test/test_x2map.c $(BUILD)/arm/libtest.a tools/x2_pad.c tools/x2_pad.h $(HDRS)
	$(ARM_CC) $(T_CFLAGS) -static -o $@ $< tools/x2_pad.c $(BUILD)/arm/libtest.a $(LDLIBS)
# The API test: the answering modem from the static library as installed
# (only v90m_* visible), the simulated caller from the dev archive. Both
# carry the modem's code; the static library's copy is local to it.
API_INC := -Iinclude -Itest -Isim -Imodem -Iv90 -Iv92 -Iv34
# the renegotiation test reaches the engines' handles: the development archive alone
$(BUILD)/api_test_reneg: test/api/test_reneg.c $(DEV) include/v90modem.h api/v90modem_priv.h
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(API_INC) -Iapi -o $@ $< $(DEV) $(LDLIBS)
# every error an engine's header defines has a name (tools/check-diag-names.sh)
.PHONY: check-diag-names
check-diag-names: $(BUILD)/api_test_diag tools/check-diag-names.sh
	@tools/check-diag-names.sh $(abspath $(BUILD))/api_test_diag
# the cross-built libraries, built once per make run, before any test program
# that links them (several of those at once, each building the same library,
# raced under -j). The archives are targets of their own, made by that one
# run, so a test program is linked again when one of them changed.
.PHONY: arm-api-libs
arm-api-libs:
	@$(MAKE) -s BUILD=$(BUILD)/arm CC=$(ARM_CC) $(BUILD)/arm/$(NAME).a $(BUILD)/arm/$(NAME)-dev.a check-symbols
ARM_API_LIBS := $(BUILD)/arm/$(NAME).a $(BUILD)/arm/$(NAME)-dev.a
$(ARM_API_LIBS): arm-api-libs ;
$(BUILD)/arm/api_test_reneg: test/api/test_reneg.c include/v90modem.h api/v90modem_priv.h $(ARM_API_LIBS)
	$(ARM_CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(API_INC) -Iapi -static -o $@ $< $(BUILD)/arm/$(NAME)-dev.a $(LDLIBS)
$(BUILD)/api_test_%: test/api/test_%.c $(STATIC) $(DEV) include/v90modem.h api/v90modem_fields.h api/v90modem_diag.h
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(API_INC) -o $@ $< $(STATIC) $(DEV) $(LDLIBS)
$(BUILD)/arm/api_test_%: test/api/test_%.c include/v90modem.h api/v90modem_fields.h api/v90modem_diag.h $(ARM_API_LIBS)
	$(ARM_CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(API_INC) -static -o $@ $< $(BUILD)/arm/$(NAME).a $(BUILD)/arm/$(NAME)-dev.a $(LDLIBS)

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do $(abspath $(BUILD))/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi
# the ARM runs of TESTS: a SPLIT_TESTS program as one run per case
# (arm_test_p34:N, its count from `prog --cases`), the others whole
ARM_ENTRIES = for t in $(TESTS); do \
	   p=$(abspath $(BUILD))/arm_$$t; \
	   case " $(SPLIT_TESTS) " in \
	   *" $$t "*) n=$$($(QEMU) $$p --cases) || { echo "$$p --cases failed" >&2; exit 1; }; n=$$(echo "$$n" | tr -d '\r'); \
	      case $$n in ''|*[!0-9]*|0) echo "$$p --cases: no case count ($$n)" >&2; exit 1 ;; esac; \
	      i=1; while [ $$i -le $$n ]; do echo $$p:$$i; i=$$((i + 1)); done ;; \
	   *) echo $$p ;; \
	   esac; done
test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@tops=$$($(ARM_ENTRIES)) || exit 1; \
	 if tools/par-run.sh $(abspath $(BUILD))/arm_log "$(QEMU)" "[arm]" $$tops; \
	 then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi
# (its output also in $(BUILD)/log/api/, for tools/golden.sh)
test-api: $(addprefix $(BUILD)/api_,$(API_TESTS))
	@mkdir -p $(BUILD)/log/api; fail=0; for t in $(API_TESTS); do \
	   $(abspath $(BUILD))/api_$$t > $(BUILD)/log/api/$$t.log 2>&1 || fail=1; cat $(BUILD)/log/api/$$t.log; \
	 done; [ $$fail -eq 0 ]
test-api-arm: $(addprefix $(BUILD)/arm/api_,$(API_TESTS))
	@fail=0; for t in $(API_TESTS); do $(QEMU) $(abspath $(BUILD))/arm/api_$$t || fail=1; done; [ $$fail -eq 0 ]

# the subdirectories build in their own build/ unless BUILD names another place
SUB_BUILD = $(if $(filter build,$(BUILD)),build,$(abspath $(BUILD))/$$d)
sub_build = $(if $(filter build,$(BUILD)),build,$(abspath $(BUILD))/$(1))
# a subdirectory's test programs, built (host, ARM); its list of them
SUB_PROGS     := $(addprefix sub-progs-,$(SUBS))
SUB_PROGS_ARM := $(addprefix sub-progs-arm-,$(SUBS))
.PHONY: $(SUB_PROGS) $(SUB_PROGS_ARM)
$(SUB_PROGS): sub-progs-%:
	@$(MAKE) -s -C $* BUILD=$(call sub_build,$*) build-tests
$(SUB_PROGS_ARM): sub-progs-arm-%:
	@$(MAKE) -s -C $* BUILD=$(call sub_build,$*) build-tests-arm ARM_CC=$(ARM_CC)
SUB_LIST = $$(for d in $(SUBS); do $(MAKE) -s --no-print-directory -C $$d BUILD=$(SUB_BUILD) $(1) || exit 1; done)

# every host program of every directory: with PAR, in one pool of PAR at a
# time (tools/par-run.sh; logs in $(BUILD)/log); else each directory in turn
ifneq ($(PAR),)
test-all: $(addprefix $(BUILD)/,$(TESTS)) $(SUB_PROGS)
	@echo "== running host tests, all directories, $(PAR) at a time =="
	@subs=$(call SUB_LIST,print-tests) || exit 1; \
	 if PAR=$(PAR) tools/par-run.sh $(abspath $(BUILD))/log "" "" $(addprefix $(abspath $(BUILD))/,$(TESTS)) $$subs; \
	 then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi
else
test-all: test
	@for d in $(SUBS); do echo "== $$d =="; $(MAKE) -s -C $$d BUILD=$(SUB_BUILD) test || exit 1; done
endif
# tools/test-times.txt from the times par-run measured in $(BUILD)
test-times:
	@tools/par-run.sh --merge $(BUILD)/test-times.txt tools/test-times.txt && echo "tools/test-times.txt: from $(BUILD)/test-times.txt"

check: test-all test-api test-examples test-fixtures check-symbols check-soname check-version check-headers check-diag-names
	@echo "ALL HOST CHECKS PASS"
# Golden outputs (tools/golden.sh): every program of make check, its output
# (less host timing) against test/golden/out/<dir>/<prog>.txt. golden runs
# make check (PAR defaults to every CPU here) and compares; golden-record
# writes them from that run instead (only when a change explains it).
# print-golden: each program's log key under $(BUILD)/log and its path.
NPROC := $(shell nproc 2>/dev/null || echo 4)
.PHONY: golden golden-record print-golden bench-hashes replay-proof
golden golden-record:
	@unset V92_VERBOSE V32_DEBUG; $(MAKE) -s check PAR=$(or $(PAR),$(NPROC)) BUILD=$(BUILD)
	@tools/golden.sh $(if $(filter golden,$@),check,record) $(BUILD)
print-golden:
	@for t in $(TESTS); do echo $$t $(abspath $(BUILD))/$$t; done
	@for d in $(SUBS); do $(MAKE) -s --no-print-directory -C $$d BUILD=$(SUB_BUILD) print-tests | sed "s|^.*/\([^/]*\)$$|$$d/\1 &|" || exit 1; done
	@for t in $(API_TESTS); do echo api/$$t $(abspath $(BUILD))/api_$$t; done
	@echo examples/call_loop $(abspath $(BUILD))/call_loop
# the benches' tx and rx hashes against test/golden/bench.txt (host only)
bench-hashes:
	@BUILD=$(BUILD) tools/bench-hashes.sh check
# per commit and per phase (docs/superpowers/specs/2026-11-22-audit-refactor-optimize-design.md 7.4)
.PHONY: verify verify-phase
verify:
	@tools/verify.sh
verify-phase:
	@$(if $(RECS),RECS=$(RECS)) tools/verify-phase.sh
# recordings of real calls (test/fixtures/fixtures.txt) through the replay tool
test-fixtures: $(BUILD)/v90m-replay $(BUILD)/rec_orig
	@echo "== real-modem recordings =="; test/fixtures/check.sh $(abspath $(BUILD))/v90m-replay
	@echo "== v90m-replay -s, -l, -r, -B, -M and -F =="; test/fixtures/replay-opts.sh $(abspath $(BUILD))/v90m-replay $(abspath $(BUILD))/rec_orig
# every ARM program of every directory in one pool (logs in $(BUILD)/arm_log);
# each case of a SPLIT_TESTS program a run of its own (arm_test_p34:N);
# ARM_SKIP=FILE leaves out the programs it lists (tools/ports-slow.txt)
ARM_SKIP ?=
check-arm: $(addprefix $(BUILD)/arm_,$(TESTS)) $(SUB_PROGS_ARM) test-api-arm
	@echo "== running ARM tests under $(QEMU), all directories =="
	@subs=$(call SUB_LIST,print-tests-arm) || exit 1; \
	 tops=$$($(ARM_ENTRIES)) || exit 1; \
	 if PAR=$(PAR) $(if $(ARM_SKIP),PAR_SKIP=$(abspath $(ARM_SKIP))) tools/par-run.sh $(abspath $(BUILD))/arm_log "$(QEMU)" "[arm]" $$tops $$subs; \
	 then echo "ALL ARM CHECKS PASS"; else echo "ARM CHECKS FAILED"; exit 1; fi
# The host suite with undefined behaviour trapped (clang's -fsanitize=undefined):
# a signed overflow or a shift of a negative number is what lets the same C
# compute differently on another target or compiler; any of them fails.
UBSAN_CC    ?= clang
UBSAN_FLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=undefined
check-ubsan:
	@$(MAKE) -s BUILD=$(BUILD)/ubsan CC=$(UBSAN_CC) CFLAGS="$(UBSAN_FLAGS)" test-all test-api test-fixtures
	@echo "ALL UBSAN CHECKS PASS"

INTEROP := interop_v8_spandsp interop_v22bis_spandsp interop_v8orig_spandsp interop_orig_v22bis_spandsp
$(BUILD)/interop_%: test/interop_%.c test/peer_spandsp.c $(T_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -Wno-unused-parameter -I$(SPANDSP)/include -o $@ $< test/peer_spandsp.c $(T_SRC) -L$(SPANDSP)/lib -Wl,-rpath,$(SPANDSP)/lib -lspandsp -ltiff $(LDLIBS)
test-interop: $(addprefix $(BUILD)/,$(INTEROP))
	@echo "== running interop tests =="
	@fail=0; for t in $(INTEROP); do $(abspath $(BUILD))/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL INTEROP TESTS PASS"; else echo "INTEROP TESTS FAILED"; exit 1; fi

# ---- the examples, through the public API only: answer a recorded call;
# call the library's own answerer (run by make check) ----
EXAMPLES := answer_file call_loop
examples: $(addprefix $(BUILD)/,$(EXAMPLES))
$(addprefix $(BUILD)/,$(EXAMPLES)): $(BUILD)/%: examples/%.c $(STATIC) include/v90modem.h
	$(CC) $(CFLAGS) -Iinclude -o $@ $< $(STATIC) $(LDLIBS)
test-examples: $(BUILD)/call_loop
	@echo "== examples/call_loop =="; mkdir -p $(BUILD)/log/examples; \
	 $(abspath $(BUILD))/call_loop > $(BUILD)/log/examples/call_loop.log 2>&1; rc=$$?; \
	 cat $(BUILD)/log/examples/call_loop.log; exit $$rc

# the 106 private recordings through v90m-replay in six flag sets against
# test/golden/replay.sha256 (V90M_NO_RECS=1 skips; tools/replay-proof.sh
# record | diff REF)
replay-proof:
	@BUILD=$(BUILD) tools/replay-proof.sh check

# ---- the replay tool (ships with the library): modem and V.42 only, built
# from the sources with the trace hooks in ----
REPLAY_SRC := tools/v90m_replay.c tools/trace.c $(LIB_SRC)
$(BUILD)/v90m-replay: $(REPLAY_SRC) $(LIB_HDR) tools/trace.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS_ALL) -DP34D_TRACE $(INC) -Itools -o $@ $(REPLAY_SRC) $(LDLIBS)
replay: $(BUILD)/v90m-replay
# K56flex's CRd detector over what the answerer heard in recorded calls
# (tools/k56_crd_scan.c): RECS=<dir> of v90m-replay recordings (*.raw);
# a detection is a false CRd. Host only, not part of check.
RECS ?=
$(BUILD)/k56-crd-scan: tools/k56_crd_scan.c modem/v8bis.c modem/v8bismsg.c modem/v21.c modem/sintab.c v90/v90_pcm.c v90/v90_pcm_tab.c $(LIB_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ tools/k56_crd_scan.c modem/v8bis.c modem/v8bismsg.c modem/v21.c modem/sintab.c v90/v90_pcm.c v90/v90_pcm_tab.c $(LDLIBS)
.PHONY: crd-scan
crd-scan: $(BUILD)/k56-crd-scan
	@if [ -z "$(RECS)" ]; then echo "RECS not set (a directory of recordings)"; exit 1; fi; \
	$(abspath $(BUILD))/k56-crd-scan $(RECS)/*.raw | tail -1
# our K56flex encoder's octets and data bits at every rate, pad and robbed
# mask (tools/k56enc_dump.c), for SM56's decoder harness outside the
# repository (docs/history/k56-facts.md "Oracle"). Host only, not part of check.
K56ENC_DUMP_SRC := tools/k56enc_dump.c modem/k56enc.c modem/k56tab.c v34/v34_shell.c v90/v90_pcm.c v90/v90_pcm_tab.c v90/v90_scr.c
$(BUILD)/k56enc-dump: $(K56ENC_DUMP_SRC) $(LIB_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ $(K56ENC_DUMP_SRC) $(LDLIBS)
.PHONY: k56-dump
k56-dump: $(BUILD)/k56enc-dump
	@mkdir -p $(BUILD)/k56dump
	$(abspath $(BUILD))/k56enc-dump $(BUILD)/k56dump
# what the caller of a recorded call sent, field by field (tools/v90m_fields.c;
# not installed): the answering modem as v90m-replay runs it, without the
# trace hooks, and the library's receivers on the caller's signal
$(BUILD)/v90m-fields: tools/v90m_fields.c $(LIB_SRC) $(LIB_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS_ALL) $(INC) -o $@ tools/v90m_fields.c $(LIB_SRC) $(LDLIBS)

# ---- V.90 on the target: record a simulated call on the host, replay it
# (rig/bench_v90.c): CPU cost per phase and a bit-exactness check; the same
# recording through the public API (rig/bench_api.c), static and shared ----
V90_REC := $(BUILD)/v90_call.rec
bench-v90: $(BUILD)/rec_v90 $(BUILD)/bench_v90 $(BUILD)/bench_v90.arm $(BUILD)/bench_api $(BUILD)/bench_api.so $(BUILD)/bench_api.arm
	$(abspath $(BUILD))/rec_v90 -55 0.2 40 0 $(V90_REC) | tail -2
	$(abspath $(BUILD))/bench_v90 $(V90_REC)
	$(QEMU) $(abspath $(BUILD))/bench_v90.arm $(V90_REC) | head -1
	$(abspath $(BUILD))/bench_api $(V90_REC)
	LD_LIBRARY_PATH=$(BUILD) $(abspath $(BUILD))/bench_api.so $(V90_REC)
	$(QEMU) $(abspath $(BUILD))/bench_api.arm $(V90_REC)
$(BUILD)/rec_v90: tools/rec_v90.c $(SIM_SRC) $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ tools/rec_v90.c $(SIM_SRC) $(MODEM_SRC) $(LDLIBS)
# bench_v90 and rec_x2 have V.42 for -e and EC (rig/ec_side.h)
$(BUILD)/bench_v90: rig/bench_v90.c rig/ec_side.h $(MODEM_SRC) $(V42_SRC) $(HDRS) $(wildcard v42/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -Iv42 -o $@ rig/bench_v90.c $(MODEM_SRC) $(V42_SRC) $(LDLIBS)
$(BUILD)/bench_v90.arm: rig/bench_v90.c rig/ec_side.h $(MODEM_SRC) $(V42_SRC) $(HDRS) $(wildcard v42/*.h)
	@mkdir -p $(BUILD)
	$(ARM_CC) $(T_CFLAGS) -Iv42 -static -o $@ rig/bench_v90.c $(MODEM_SRC) $(V42_SRC) $(LDLIBS)
	$(dir $(ARM_CC))arm-linux-strip $@
$(BUILD)/bench_api: rig/bench_api.c $(STATIC)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< $(STATIC) $(LDLIBS)
$(BUILD)/bench_api.so: rig/bench_api.c $(SHARED)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< -L$(BUILD) -lv90modem $(LDLIBS)
$(BUILD)/bench_api.arm: rig/bench_api.c include/v90modem.h
	$(MAKE) -s BUILD=$(BUILD)/arm CC=$(ARM_CC) $(BUILD)/arm/$(NAME).a
	$(ARM_CC) $(CFLAGS) -Iinclude -static -o $@ $< $(BUILD)/arm/$(NAME).a $(LDLIBS)

# ---- x2 answering: the same with a call of the simulated x2 client
# (tools/rec_x2, replayed by bench_v90 -x); cost-x2 counts ARM instructions
# per second of data mode for x2 and for V.90, as cost-v34 does ----
X2_REC := $(BUILD)/x2_call.rec
# rec_x2's arguments after its file: INDEX (1: 32000, 15: 56000) and EC
# (none|v42bis|v44: V.42 on the data bits, which bench_v90 then replays
# with -e), e.g. X2_ARGS="15 v42bis"
X2_ARGS ?=
X2_EC = $(filter none v42bis v44,$(X2_ARGS))
bench-x2: $(BUILD)/rec_x2 $(BUILD)/bench_v90 $(BUILD)/bench_v90.arm
	$(abspath $(BUILD))/rec_x2 40 0 $(X2_REC) $(X2_ARGS) | tail -3
	$(abspath $(BUILD))/bench_v90 -x $(if $(X2_EC),-e $(X2_EC)) $(X2_REC)
	$(QEMU) $(abspath $(BUILD))/bench_v90.arm -x $(if $(X2_EC),-e $(X2_EC)) $(X2_REC)
$(BUILD)/rec_x2: tools/rec_x2.c rig/ec_side.h $(SIM_SRC) $(MODEM_SRC) $(V42_SRC) $(HDRS) $(wildcard v42/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -Iv42 -o $@ tools/rec_x2.c $(SIM_SRC) $(MODEM_SRC) $(V42_SRC) $(LDLIBS)
.PHONY: bench-x2 cost-x2
cost-x2: bench-x2 bench-v90
	@head -c $$((8000 * 15)) $(X2_REC) > $(BUILD)/x2_a.rec; head -c $$((8000 * 16)) $(X2_REC) > $(BUILD)/x2_b.rec; \
	 head -c $$((8000 * 15)) $(V90_REC) > $(BUILD)/v90_c.rec; head -c $$((8000 * 16)) $(V90_REC) > $(BUILD)/v90_d.rec; \
	 for r in x2_a x2_b v90_c v90_d; do \
	   case $$r in x2*) x=-x;; *) x=;; esac; \
	   ( $(CNT) $(abspath $(BUILD))/bench_v90.arm $$x $(BUILD)/$$r.rec 2>&1 | grep -c '^Trace' > $(BUILD)/$$r.cnt ) & \
	 done; wait; \
	 a=$$(cat $(BUILD)/x2_a.cnt); b=$$(cat $(BUILD)/x2_b.cnt); c=$$(cat $(BUILD)/v90_c.cnt); d=$$(cat $(BUILD)/v90_d.cnt); \
	 echo "x2 data mode: $$((b - a)) ARM instructions per second of line (second 15 to 16 of the call)"; \
	 echo "V.90 data mode: $$((d - c)) ARM instructions per second of line (second 15 to 16 of the call)"; \
	 echo "ratio x2 / V.90: $$(( (b - a) * 100 / (d - c) )) %"

# cost-x2-v42 counts x2 data mode as cost-x2 does (second 15 to 16), with
# our V.42 endpoint on the answerer's data bits (rig/ec_side.h): no
# compression, V.42 bis, V.44, at 32000 (cost-x2's rate) and 56000. The
# client writes an English sentence both ways; its recordings are its own
# (x2e_INDEX_EC.rec), and the host's and target's replays must agree ----
.PHONY: cost-x2-v42
cost-x2-v42: $(BUILD)/rec_x2 $(BUILD)/bench_v90 $(BUILD)/bench_v90.arm
	@for i in 1 15; do for e in none v42bis v44; do \
	   r=$(BUILD)/x2e_$${i}_$$e; \
	   $(abspath $(BUILD))/rec_x2 40 0 $$r.rec $$i $$e > $$r.log || { cat $$r.log; exit 1; }; \
	   h=$$($(abspath $(BUILD))/bench_v90 -x -e $$e $$r.rec | sed -n '/^[tr]x hash\|^V.42/p'); \
	   t=$$($(QEMU) $(abspath $(BUILD))/bench_v90.arm -x -e $$e $$r.rec | sed -n '/^[tr]x hash\|^V.42/p'); \
	   [ "$$h" = "$$t" ] || { echo "bench_v90 -x -e $$e ($$i): host '$$h', target '$$t'"; exit 1; }; \
	   grep -q "^transmit hash $$(echo "$$h" | sed -n 's/^tx hash //p')," $$r.log || { echo "bench_v90 -x -e $$e ($$i): not the recorder's transmit hash"; exit 1; }; \
	   head -c $$((8000 * 15)) $$r.rec > $$r.a; head -c $$((8000 * 16)) $$r.rec > $$r.b; \
	 done; done; \
	 for i in 1 15; do for e in none v42bis v44; do for p in a b; do \
	   r=$(BUILD)/x2e_$${i}_$$e; \
	   ( $(CNT) $(abspath $(BUILD))/bench_v90.arm -x -e $$e $$r.$$p 2>&1 | grep -c '^Trace' > $$r.$$p.cnt ) & \
	 done; done; done; wait; \
	 for i in 1 15; do for e in none v42bis v44; do \
	   r=$(BUILD)/x2e_$${i}_$$e; a=$$(cat $$r.a.cnt); b=$$(cat $$r.b.cnt); \
	   echo "x2 data mode at $$(sed -n 's/^x2 \([0-9]*\) down.*/\1/p' $$r.log), V.42 $$e: $$((b - a)) ARM instructions per second of line (second 15 to 16 of the call)"; \
	 done; done

# ---- K56flex answering: the same with a call of the simulated K56flex
# client (tools/rec_k56, 56000 down, replayed by bench_v90 -k); cost-k56
# counts ARM instructions per second of data mode for K56flex (from second
# 20: its data mode starts at 16.2 s, after V.8bis) and for V.90, as cost-x2
# does ----
K56_REC := $(BUILD)/k56_call.rec
# rec_k56's options after its file: K56_ARGS=heavy, a client that ignores our
# MP (x2's upstream: 64 states, non-linear, expanded shaping)
K56_ARGS ?=
bench-k56: $(BUILD)/rec_k56 $(BUILD)/bench_v90 $(BUILD)/bench_v90.arm
	$(abspath $(BUILD))/rec_k56 40 0 $(K56_REC) $(K56_ARGS) | tail -2
	$(abspath $(BUILD))/bench_v90 -k $(K56_REC)
	$(QEMU) $(abspath $(BUILD))/bench_v90.arm -k $(K56_REC)
$(BUILD)/rec_k56: tools/rec_k56.c $(SIM_SRC) $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ tools/rec_k56.c $(SIM_SRC) $(MODEM_SRC) $(LDLIBS)
.PHONY: bench-k56 cost-k56
cost-k56: bench-k56 bench-v90
	@head -c $$((8000 * 20)) $(K56_REC) > $(BUILD)/k56_a.rec; head -c $$((8000 * 21)) $(K56_REC) > $(BUILD)/k56_b.rec; \
	 head -c $$((8000 * 15)) $(V90_REC) > $(BUILD)/v90_c.rec; head -c $$((8000 * 16)) $(V90_REC) > $(BUILD)/v90_d.rec; \
	 for r in k56_a k56_b v90_c v90_d; do \
	   case $$r in k56*) x=-k;; *) x=;; esac; \
	   ( $(CNT) $(abspath $(BUILD))/bench_v90.arm $$x $(BUILD)/$$r.rec 2>&1 | grep -c '^Trace' > $(BUILD)/$$r.cnt ) & \
	 done; wait; \
	 a=$$(cat $(BUILD)/k56_a.cnt); b=$$(cat $(BUILD)/k56_b.cnt); c=$$(cat $(BUILD)/v90_c.cnt); d=$$(cat $(BUILD)/v90_d.cnt); \
	 echo "k56 data mode: $$((b - a)) ARM instructions per second of line (second 20 to 21 of the call)"; \
	 echo "V.90 data mode: $$((d - c)) ARM instructions per second of line (second 15 to 16 of the call)"; \
	 echo "ratio k56 / V.90: $$(( (b - a) * 100 / (d - c) )) %"

# ---- V.34 answering: record a simulated call, replay it on host and ARM ----
V34_REC ?= $(BUILD)/v34_call.rec
# cost-v34 counts ARM instructions per second of data mode for V.34 and, the
# same way, for V.90 (whose data mode was timed on the real box), so the two
# can be compared. Slow (minutes): qemu logs every instruction.
CNT = $(QEMU) -one-insn-per-tb -d exec,nochain
.PHONY: cost-v34
cost-v34: bench-v34 bench-v90
	@head -c $$((2 * 8000 * 10)) $(V34_REC) > $(BUILD)/v34_a.rec; head -c $$((2 * 8000 * 11)) $(V34_REC) > $(BUILD)/v34_b.rec; \
	 head -c $$((8000 * 10)) $(V90_REC) > $(BUILD)/v90_a.rec; head -c $$((8000 * 11)) $(V90_REC) > $(BUILD)/v90_b.rec; \
	 a=$$($(CNT) $(abspath $(BUILD))/bench_v34.arm $(BUILD)/v34_a.rec 2>&1 | grep -c '^Trace'); \
	 b=$$($(CNT) $(abspath $(BUILD))/bench_v34.arm $(BUILD)/v34_b.rec 2>&1 | grep -c '^Trace'); \
	 c=$$($(CNT) $(abspath $(BUILD))/bench_v90.arm $(BUILD)/v90_a.rec 2>&1 | grep -c '^Trace'); \
	 d=$$($(CNT) $(abspath $(BUILD))/bench_v90.arm $(BUILD)/v90_b.rec 2>&1 | grep -c '^Trace'); \
	 echo "V.34 data mode: $$((b - a)) ARM instructions per second of line (second 10 to 11 of the call)"; \
	 echo "V.90 data mode: $$((d - c)) ARM instructions per second of line (second 10 to 11 of the call)"; \
	 echo "ratio V.34 / V.90: $$(( (b - a) * 100 / (d - c) )) %"
bench-v34: $(BUILD)/rec_v34 $(BUILD)/bench_v34 $(BUILD)/bench_v34.arm
	$(abspath $(BUILD))/rec_v34 16 $(V34_REC)
	$(abspath $(BUILD))/bench_v34 $(V34_REC)
	$(QEMU) $(abspath $(BUILD))/bench_v34.arm $(V34_REC) | head -1
$(BUILD)/rec_v34: tools/rec_v34.c rig/v34glue.h $(SIM_SRC) $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ tools/rec_v34.c $(SIM_SRC) $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v34: rig/bench_v34.c rig/v34glue.h $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ rig/bench_v34.c $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v34.arm: rig/bench_v34.c rig/v34glue.h $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(ARM_CC) $(T_CFLAGS) -static -o $@ rig/bench_v34.c $(MODEM_SRC) $(LDLIBS)

# ---- V.90 analogue modem, the downstream receiver (modem/v90arx): record a
# simulated downstream on the host (tools/rec_v90a: 2 km, +100 ppm, echo of
# our V.34 upstream 8 dB over the downstream, 48000 bit/s), replay it on the
# host and under qemu; the decision hash must agree ----
V90A_REC := $(BUILD)/v90a.rec
bench-v90a: $(BUILD)/rec_v90a $(BUILD)/bench_v90a $(BUILD)/bench_v90a.arm
	$(abspath $(BUILD))/rec_v90a 100 48000 $(V90A_REC)
	$(abspath $(BUILD))/bench_v90a $(V90A_REC)
	$(QEMU) $(abspath $(BUILD))/bench_v90a.arm $(V90A_REC)
# cost-v90a counts ARM instructions as cost-v34 does: per second of line in
# data mode (seconds 3 to 4; data from 1.2 s), and over Sd and TRN1d
# (0.55 to 1.25 s: the clock measured, the estimate, the first computation
# of the equaliser), at 0 ppm (the equaliser computed again every 2048
# symbols) and 100 ppm (every 512). Slow: qemu logs every instruction.
.PHONY: cost-v90a
cost-v90a: $(BUILD)/rec_v90a $(BUILD)/bench_v90a.arm
	@for p in 0 100; do \
	   $(abspath $(BUILD))/rec_v90a $$p 48000 $(BUILD)/v90a_$$p.rec > /dev/null; \
	   for s in 4400 10000 24000 32000; do head -c $$((36 + 4 * s)) $(BUILD)/v90a_$$p.rec > $(BUILD)/v90a_$$p.$$s; done; \
	   a=$$($(CNT) $(abspath $(BUILD))/bench_v90a.arm $(BUILD)/v90a_$$p.4400 2>&1 | grep -c '^Trace'); \
	   b=$$($(CNT) $(abspath $(BUILD))/bench_v90a.arm $(BUILD)/v90a_$$p.10000 2>&1 | grep -c '^Trace'); \
	   c=$$($(CNT) $(abspath $(BUILD))/bench_v90a.arm $(BUILD)/v90a_$$p.24000 2>&1 | grep -c '^Trace'); \
	   d=$$($(CNT) $(abspath $(BUILD))/bench_v90a.arm $(BUILD)/v90a_$$p.32000 2>&1 | grep -c '^Trace'); \
	   echo "V.90 downstream receiver, $$p ppm: data mode $$((d - c)) ARM instructions per second of line (second 3 to 4);" \
	        "Sd and TRN1d (0.7 s) $$((b - a))"; \
	 done
$(BUILD)/rec_v90a: tools/rec_v90a.c $(SIM_SRC) $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ tools/rec_v90a.c $(SIM_SRC) $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v90a: rig/bench_v90a.c $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ rig/bench_v90a.c $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v90a.arm: rig/bench_v90a.c $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(ARM_CC) $(T_CFLAGS) -static -o $@ rig/bench_v90a.c $(MODEM_SRC) $(LDLIBS)

# ---- x2 against an emulated U.S. Robotics Courier (tools/x2-peer/README.md):
# courier-emu and the ROM live outside the repository; not part of check
COURIER_EMU ?=
PAD  ?= 0
MODS ?=
STRICT_RENEG ?= 0
$(BUILD)/x2-bridge: tools/x2-peer/bridge.c $(STATIC) include/v90modem.h
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -Iinclude -o $@ $< $(STATIC) $(LDLIBS)
test-x2-courier:
	@if [ -z "$(COURIER_EMU)" ]; then echo "COURIER_EMU not set"; exit 0; fi; \
	$(MAKE) -s $(BUILD)/x2-bridge && \
	COURIER_EMU="$(COURIER_EMU)" PAD="$(PAD)" MODS="$(MODS)" STRICT_RENEG="$(STRICT_RENEG)" sh tools/x2-peer/run.sh $(abspath $(BUILD))/x2-bridge $(abspath $(BUILD))/x2-peer

# ---- K56flex against Motorola's SM56 (tools/k56-peer/README.md): D-Modem's
# half_sm56 and sm56.ko live outside the repository; not part of check
DMODEM ?=
RBS  ?= 0
DOWN ?= 0
$(BUILD)/k56-bridge: tools/k56-peer/bridge.c $(STATIC) include/v90modem.h
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -Iinclude -o $@ $< $(STATIC) $(LDLIBS)
test-k56-sm56:
	@if [ -z "$(DMODEM)" ]; then echo "DMODEM not set"; exit 0; fi; \
	if ! printf 'int main(void) { return 0; }\n' | $(CC) -m32 -x c - -o /dev/null 2>/dev/null; then \
		echo "no 32-bit toolchain (glibc-devel.i686)"; exit 0; fi; \
	$(MAKE) -s $(BUILD)/k56-bridge && \
	DMODEM="$(DMODEM)" MODS="$(MODS)" RBS="$(RBS)" DOWN="$(DOWN)" STRICT_RENEG="$(STRICT_RENEG)" sh tools/k56-peer/run.sh $(abspath $(BUILD))/k56-bridge $(abspath $(BUILD))/k56-peer

clean:
	rm -rf $(BUILD)
	@for d in $(SUBS); do $(MAKE) -s -C $$d BUILD=build clean; done

# ---- V.90 analogue modem, the whole caller (modem/orig, modem/v90a): record
# what its A/D receives in a simulated call against the library's answerer
# (tools/rec_v90c: 2 km, +100 ppm, until 5 s after data mode), replay it on
# the host and under qemu; the transmit hash must agree ----
V90C_REC := $(BUILD)/v90c.rec
bench-v90c: $(BUILD)/rec_v90c $(BUILD)/bench_v90c $(BUILD)/bench_v90c.arm
	$(abspath $(BUILD))/rec_v90c 100 5 $(V90C_REC)
	$(abspath $(BUILD))/bench_v90c $(V90C_REC)
	$(QEMU) $(abspath $(BUILD))/bench_v90c.arm $(V90C_REC)
# cost-v90c counts ARM instructions as cost-v90a does: per second of line in
# data mode (seconds 2 to 3 after data mode began), at 0 and 100 ppm; then
# each of the 4 ticks that took longest on the host (100 ppm), counted alone
# (the recording cut just after it, less the recording cut just before it).
# Slow: qemu logs every instruction.
.PHONY: cost-v90c
cost-v90c: $(BUILD)/rec_v90c $(BUILD)/bench_v90c $(BUILD)/bench_v90c.arm
	@for p in 0 100; do \
	   $(abspath $(BUILD))/rec_v90c $$p 4 $(BUILD)/v90c_$$p.rec > /dev/null; \
	   d=$$(od -An -tu4 -j8 -N4 $(BUILD)/v90c_$$p.rec | tr -d ' '); \
	   head -c $$((12 + 2 * (d + 16000))) $(BUILD)/v90c_$$p.rec > $(BUILD)/v90c_$$p.a; \
	   head -c $$((12 + 2 * (d + 24000))) $(BUILD)/v90c_$$p.rec > $(BUILD)/v90c_$$p.b; \
	   a=$$($(CNT) $(abspath $(BUILD))/bench_v90c.arm $(BUILD)/v90c_$$p.a 2>&1 | grep -c '^Trace'); \
	   b=$$($(CNT) $(abspath $(BUILD))/bench_v90c.arm $(BUILD)/v90c_$$p.b 2>&1 | grep -c '^Trace'); \
	   echo "V.90 caller, $$p ppm: data mode $$((b - a)) ARM instructions per second of line (seconds 2 to 3 of data mode)"; \
	 done; \
	 if [ "$(COST_TICKS)" != 0 ]; then \
	  for t in $$($(abspath $(BUILD))/bench_v90c $(BUILD)/v90c_100.rec -w 4 | sed -n 's/^slow tick \([0-9]*\) .*/\1/p'); do \
	   head -c $$((12 + 2 * t)) $(BUILD)/v90c_100.rec > $(BUILD)/v90c_t.a; \
	   head -c $$((12 + 2 * (t + 1))) $(BUILD)/v90c_100.rec > $(BUILD)/v90c_t.b; \
	   a=$$($(CNT) $(abspath $(BUILD))/bench_v90c.arm $(BUILD)/v90c_t.a 2>&1 | grep -c '^Trace'); \
	   b=$$($(CNT) $(abspath $(BUILD))/bench_v90c.arm $(BUILD)/v90c_t.b 2>&1 | grep -c '^Trace'); \
	   echo "V.90 caller, 100 ppm: tick $$t ($$((t / 8)) ms): $$((b - a)) ARM instructions"; \
	 done; fi
# ---- the same for the caller in V.92 (PCM upstream): rec_v90c ... v92 records
# a V.92 call (the upstream through the answerer's own V.92 line), and
# bench_v90c replays it with V.92 on ----
V92C_REC := $(BUILD)/v92c.rec
bench-v92c: $(BUILD)/rec_v90c $(BUILD)/bench_v90c $(BUILD)/bench_v90c.arm
	$(abspath $(BUILD))/rec_v90c 100 5 $(V92C_REC) v92
	$(abspath $(BUILD))/bench_v90c $(V92C_REC)
	$(QEMU) $(abspath $(BUILD))/bench_v90c.arm $(V92C_REC)
.PHONY: cost-v92c
cost-v92c: $(BUILD)/rec_v90c $(BUILD)/bench_v90c $(BUILD)/bench_v90c.arm
	@for p in 0 100; do \
	   $(abspath $(BUILD))/rec_v90c $$p 4 $(BUILD)/v92c_$$p.rec v92 > /dev/null; \
	   d=$$(od -An -tu4 -j8 -N4 $(BUILD)/v92c_$$p.rec | tr -d ' '); \
	   head -c $$((12 + 2 * (d + 16000))) $(BUILD)/v92c_$$p.rec > $(BUILD)/v92c_$$p.a; \
	   head -c $$((12 + 2 * (d + 24000))) $(BUILD)/v92c_$$p.rec > $(BUILD)/v92c_$$p.b; \
	   a=$$($(CNT) $(abspath $(BUILD))/bench_v90c.arm $(BUILD)/v92c_$$p.a 2>&1 | grep -c '^Trace'); \
	   b=$$($(CNT) $(abspath $(BUILD))/bench_v90c.arm $(BUILD)/v92c_$$p.b 2>&1 | grep -c '^Trace'); \
	   echo "V.92 caller, $$p ppm: data mode $$((b - a)) ARM instructions per second of line (seconds 2 to 3 of data mode)"; \
	 done; \
	 if [ "$(COST_TICKS)" != 0 ]; then \
	  for t in $$($(abspath $(BUILD))/bench_v90c $(BUILD)/v92c_100.rec -w 6 | sed -n 's/^slow tick \([0-9]*\) .*/\1/p'); do \
	   head -c $$((12 + 2 * t)) $(BUILD)/v92c_100.rec > $(BUILD)/v92c_t.a; \
	   head -c $$((12 + 2 * (t + 1))) $(BUILD)/v92c_100.rec > $(BUILD)/v92c_t.b; \
	   a=$$($(CNT) $(abspath $(BUILD))/bench_v90c.arm $(BUILD)/v92c_t.a 2>&1 | grep -c '^Trace'); \
	   b=$$($(CNT) $(abspath $(BUILD))/bench_v90c.arm $(BUILD)/v92c_t.b 2>&1 | grep -c '^Trace'); \
	   echo "V.92 caller, 100 ppm: tick $$t ($$((t / 8)) ms): $$((b - a)) ARM instructions"; \
	 done; fi
# COST_TICKS=0: cost-v90c and cost-v92c without the slowest ticks counted
# one by one (which ticks are slowest depends on the host's load)
COST_TICKS ?= 1
# prof-v90c / prof-v92c: the cost of every tick and the profile, from one qemu
# run each (much faster than cost-*: no -one-insn-per-tb). rec_v90c records a
# call at PPM (default 100) with 4 s of data mode; bench_v90c -t replays it
# up to 3 s into data mode, calling a mark before every tick; tools/qprof
# turns qemu's log (blocks as translated, each block as run) into the
# instructions of each tick (the heaviest listed) and per function over
# seconds 2 to 3 of data mode. QPROF_TB=N lists the N heaviest blocks too.
PPM ?= 100
.PHONY: prof-v90c prof-v92c
prof-v90c prof-v92c: $(BUILD)/rec_v90c $(BUILD)/bench_v90c.arm $(BUILD)/qprof
	@r=$(BUILD)/$@_$(PPM).rec; \
	 $(abspath $(BUILD))/rec_v90c $(PPM) 4 $$r $(if $(filter prof-v92c,$@),v92) > /dev/null; \
	 d=$$(od -An -tu4 -j8 -N4 $$r | tr -d ' '); \
	 pc=$$(nm $(BUILD)/bench_v90c.arm | sed -n 's/^\([0-9a-f]*\) T bench_v90c_mark$$/\1/p'); \
	 $(QEMU) -d in_asm,exec,nochain -D /dev/stdout $(abspath $(BUILD))/bench_v90c.arm $$r -t -n $$((d + 24000)) 2>/dev/null \
	 | $(abspath $(BUILD))/qprof $$pc $$((d + 16000)) $$((d + 24000)) -top 12 $(if $(QPROF_TB),-tb $(QPROF_TB))
$(BUILD)/qprof: tools/qprof.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<
# ---- the caller in Bell 103 and Bell 212A: rec_bell records a call against
# the library's answerer, bench_bell replays it into the caller alone (hash on
# the host and under qemu); cost-bell counts the ARM instructions of the
# first second of data mode, one qemu run per call (split where the trace
# enters bench_bell_mark) ----
.PHONY: bench-bell cost-bell
bench-bell: $(BUILD)/rec_bell $(BUILD)/bench_bell $(BUILD)/bench_bell.arm
	@for m in 103 212; do \
	   $(abspath $(BUILD))/rec_bell $$m 3 $(BUILD)/bell_$$m.rec; \
	   $(abspath $(BUILD))/bench_bell $(BUILD)/bell_$$m.rec; \
	   $(QEMU) $(abspath $(BUILD))/bench_bell.arm $(BUILD)/bell_$$m.rec; \
	 done
cost-bell: $(BUILD)/rec_bell $(BUILD)/bench_bell.arm
	@for m in 103 212; do \
	   $(abspath $(BUILD))/rec_bell $$m 2 $(BUILD)/bell_$$m.rec > /dev/null; \
	   pc=$$(nm $(BUILD)/bench_bell.arm | sed -n 's/^\([0-9a-f]*\) T bench_bell_mark$$/\1/p'); \
	   $(CNT) $(abspath $(BUILD))/bench_bell.arm $(BUILD)/bell_$$m.rec 2>&1 >/dev/null \
	   | awk -v m=$$m -v pc="/00000000$$pc/" 'BEGIN { k = 0 } /^Trace/ { if (index($$0, pc)) k++; n[k]++ } END { \
	       printf "Bell %s caller: %d ARM instructions to data mode, %d in its first second, %d after\n", m == 212 ? "212A" : "103", n[0], n[1], n[2] }'; \
	 done
$(BUILD)/rec_bell: tools/rec_bell.c $(SIM_SRC) $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ tools/rec_bell.c $(SIM_SRC) $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_bell: rig/bench_bell.c $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ rig/bench_bell.c $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_bell.arm: rig/bench_bell.c $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(ARM_CC) $(T_CFLAGS) -static -o $@ rig/bench_bell.c $(MODEM_SRC) $(LDLIBS)
$(BUILD)/rec_v90c: tools/rec_v90c.c test/v90link.h $(SIM_SRC) $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -Itest -o $@ tools/rec_v90c.c $(SIM_SRC) $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v90c: rig/bench_v90c.c $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ rig/bench_v90c.c $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v90c.arm: rig/bench_v90c.c $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(ARM_CC) $(T_CFLAGS) -static -o $@ rig/bench_v90c.c $(MODEM_SRC) $(LDLIBS)
# ---- V.42 on the target: rig/bench_v42 runs two endpoints back to back,
# both directions full of text; cost-v42 counts the ARM instructions of
# 56000 more line bits each way (one second at 56 kbit/s), the difference of
# two runs, halved: per endpoint, for no compression, V.42 bis and V.44 (the
# harness's loop included). The host's output and the target's must agree ----
.PHONY: cost-v42
cost-v42: $(BUILD)/bench_v42 $(BUILD)/bench_v42.arm
	@for c in 0 1 2; do \
	   h=$$($(abspath $(BUILD))/bench_v42 56000 $$c); a=$$($(QEMU) $(abspath $(BUILD))/bench_v42.arm 56000 $$c); \
	   [ "$$h" = "$$a" ] || { echo "bench_v42 $$c: host '$$h', target '$$a'"; exit 1; }; \
	   ( $(CNT) $(abspath $(BUILD))/bench_v42.arm 0 $$c 2>&1 | grep -c '^Trace' > $(BUILD)/v42_$${c}_a.cnt ) & \
	   ( $(CNT) $(abspath $(BUILD))/bench_v42.arm 56000 $$c 2>&1 | grep -c '^Trace' > $(BUILD)/v42_$${c}_b.cnt ) & \
	   wait; \
	   a=$$(cat $(BUILD)/v42_$${c}_a.cnt); b=$$(cat $(BUILD)/v42_$${c}_b.cnt); \
	   case $$c in 0) n="no compression";; 1) n="V.42 bis";; *) n="V.44";; esac; \
	   echo "V.42, $$n: $$(( (b - a) / 2 )) ARM instructions per endpoint per second at 56 kbit/s both ways ($$h)"; \
	 done
$(BUILD)/bench_v42: rig/bench_v42.c $(V42_SRC) $(wildcard v42/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(EXTRA_CPPFLAGS) -Iv42 -o $@ rig/bench_v42.c $(V42_SRC) $(LDLIBS)
$(BUILD)/bench_v42.arm: rig/bench_v42.c $(V42_SRC) $(wildcard v42/*.h)
	@mkdir -p $(BUILD)
	$(ARM_CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(EXTRA_CPPFLAGS) -Iv42 -static -o $@ rig/bench_v42.c $(V42_SRC) $(LDLIBS)
# a simulated call of the library's caller, recorded at the answerer as v90d
# records real ones (tools/rec_orig.c; host only)
$(BUILD)/rec_orig: tools/rec_orig.c $(SIM_SRC) $(MODEM_SRC) $(API_SRC) api/v90modem_diag.h $(HDRS) test/v90link.h test/line34.h
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -Iinclude -Iapi -o $@ tools/rec_orig.c $(API_SRC) $(SIM_SRC) $(MODEM_SRC) $(addprefix v42/,v42_hdlc.c v42_detect.c v42_comp.c v44_comp.c v42_lapm.c v42.c) $(LDLIBS)
