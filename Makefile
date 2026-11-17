# ns-autoverify sysmodule. Needs devkitA64 + libnx (DEVKITPRO) for the console build; `make test` needs only a
# host C compiler. Outputs (two separate SD trees so Stage 1 cannot ship the boot2 flag by accident):
#   out/stage1/atmosphere/contents/4200000000004156/{exefs.nsp,toolbox.json}             (no boot2.flag)
#   out/stage2/atmosphere/contents/4200000000004156/flags/boot2.flag                    (the only Stage 2 addition)
#   make            # stage1 tree
#   make stage2     # stage2 tree (adds boot2.flag only)
#   make test       # host tests of source/policy.c
#   make check-host # also type-checks main.c against libnx headers with host clang (no link, no devkitPro)
TITLE_ID := 4200000000004156
NAME     := ns-autoverify

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
LDFLAGS  := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,out/$(NAME).map -L$(LIBNX)/lib
SRCS     := source/main.c source/policy.c
OBJS     := $(patsubst %.c,out/obj/%.o,$(SRCS))
S1       := out/stage1/atmosphere/contents/$(TITLE_ID)
S2       := out/stage2/atmosphere/contents/$(TITLE_ID)

all: $(S1)/exefs.nsp $(S1)/toolbox.json

stage2: $(S2)/flags/boot2.flag

out/obj/%.o: %.c source/policy.h
	@mkdir -p $(dir $@)
	$(A64)gcc $(CFLAGS) -c $< -o $@

out/$(NAME).elf: $(OBJS)
	$(A64)gcc $(LDFLAGS) $(OBJS) -lnx -o $@

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

test:
	@mkdir -p out
	cc -std=c11 -O1 -Wall -Wextra -Werror -Isource source/policy.c tests/test_policy.c -o out/test_policy
	./out/test_policy

# Type-check only; LIBNX_SRC points at a libnx source checkout (its nx/include). Needs tests/shim for the two
# newlib-only headers libnx includes. Not a substitute for the devkitA64 build.
LIBNX_SRC ?= /home/intpa/ssbu-switch-remote-research/libnx
check-host:
	clang --target=aarch64-linux-gnu -std=gnu11 -fsyntax-only -Wall -Wextra -Werror -D__SWITCH__ -DAV_LIBNX_NEW_PGL_NAMES \
	  -isystem tests/shim -isystem $(LIBNX_SRC)/nx/include -idirafter /usr/include/x86_64-linux-gnu \
	  -Isource source/main.c source/policy.c

clean:
	rm -rf out

.PHONY: all stage2 test check-host clean
.DEFAULT_GOAL := all
