# ns-autoverify (fast clear) sysmodule + nsav-probe NRO. Needs devkitA64 + libnx (DEVKITPRO) for console builds;
# `make test` needs only a host C compiler. Outputs (separate SD trees so Stage 1 cannot ship boot2.flag by accident):
#   out/probe/switch/nsav-probe/nsav-probe.nro                                       (the probe, run from hbmenu)
#   out/stage1/atmosphere/contents/4200000000004156/{exefs.nsp,toolbox.json}         (no boot2.flag)
#   out/stage2/atmosphere/contents/4200000000004156/flags/boot2.flag                (the only Stage 2 addition)
#   make            # stage1 tree + probe
#   make stage2     # stage2 tree (adds boot2.flag only)
#   make test       # host tests of source/policy.c
#   make check-host # type-check all C against libnx headers with host clang (no link, no devkitPro)
TITLE_ID := 4200000000004156
NAME     := ns-autoverify
PROBE    := nsav-probe
VERSION  := 0.4.0

NEEDS_DKP := $(filter-out test check-host clean,$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all))
ifneq ($(NEEDS_DKP),)
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set: install devkitA64 + libnx (devkitPro pacman group switch-dev) or build on a host that has it)
endif
endif

A64      := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
TOOLS    := $(DEVKITPRO)/tools/bin
LIBNX    := $(DEVKITPRO)/libnx
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -O2 -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
            $(ARCH) -D__SWITCH__ -isystem $(LIBNX)/include -Isource $(EXTRA_CFLAGS)
LDFLAGS  := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,--gc-sections -L$(LIBNX)/lib
SYS_SRCS := source/main.c source/nsx.c source/policy.c
PRB_SRCS := probe/probe.c source/nsx.c source/policy.c
SYS_OBJS := $(patsubst %.c,out/obj/%.o,$(SYS_SRCS))
PRB_OBJS := $(patsubst %.c,out/obj/%.o,$(PRB_SRCS))
S1       := out/stage1/atmosphere/contents/$(TITLE_ID)
S2       := out/stage2/atmosphere/contents/$(TITLE_ID)
PT       := out/probe/switch/$(PROBE)

all: $(S1)/exefs.nsp $(S1)/toolbox.json $(PT)/$(PROBE).nro

probe: $(PT)/$(PROBE).nro

stage2: $(S2)/flags/boot2.flag

out/obj/%.o: %.c source/policy.h source/nsx.h
	@mkdir -p $(dir $@)
	$(A64)gcc $(CFLAGS) -c $< -o $@

out/$(NAME).elf: $(SYS_OBJS)
	$(A64)gcc $(LDFLAGS) -Wl,-Map,out/$(NAME).map $(SYS_OBJS) -lnx -o $@

out/$(NAME).nso: out/$(NAME).elf
	$(TOOLS)/elf2nso $< $@

out/$(NAME).npdm: $(NAME).json
	@mkdir -p out
	$(TOOLS)/npdmtool $< $@

out/exefs.nsp: out/$(NAME).nso out/$(NAME).npdm
	rm -rf out/exefs && mkdir -p out/exefs
	cp out/$(NAME).nso out/exefs/main
	cp out/$(NAME).npdm out/exefs/main.npdm
	$(TOOLS)/build_pfs0 out/exefs $@

$(S1)/exefs.nsp: out/exefs.nsp
	@mkdir -p $(dir $@)
	cp $< $@

$(S1)/toolbox.json: toolbox.json
	@mkdir -p $(dir $@)
	cp $< $@

$(S2)/flags/boot2.flag:
	@mkdir -p $(dir $@)
	touch $@

out/$(PROBE).elf: $(PRB_OBJS)
	$(A64)gcc $(LDFLAGS) -Wl,-Map,out/$(PROBE).map $(PRB_OBJS) -lnx -o $@

out/$(PROBE).nacp:
	@mkdir -p out
	$(TOOLS)/nacptool --create "$(PROBE)" "switch-ns-autoverify" "$(VERSION)" $@

$(PT)/$(PROBE).nro: out/$(PROBE).elf out/$(PROBE).nacp
	@mkdir -p $(dir $@)
	$(TOOLS)/elf2nro out/$(PROBE).elf $@ --nacp=out/$(PROBE).nacp

test:
	@mkdir -p out
	cc -std=c11 -O1 -Wall -Wextra -Werror -Isource source/policy.c tests/test_policy.c -o out/test_policy
	./out/test_policy

# Type-check only; LIBNX_SRC points at a libnx source checkout. tests/shim stands in for newlib-only headers.
LIBNX_SRC ?= /home/intpa/ssbu-switch-remote-research/libnx
check-host:
	clang --target=aarch64-linux-gnu -std=gnu11 -fsyntax-only -Wall -Wextra -Werror -Wno-c23-extensions \
	  -D__SWITCH__ -DAV_LIBNX_NEW_PGL_NAMES \
	  -isystem tests/shim -isystem $(LIBNX_SRC)/nx/include -idirafter /usr/include/x86_64-linux-gnu \
	  -Isource source/main.c source/nsx.c source/policy.c probe/probe.c

clean:
	rm -rf out

.PHONY: all probe stage2 test check-host clean
.DEFAULT_GOAL := all
