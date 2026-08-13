#################################
# RISCV Toolchain
#################################

PREFIX = riscv64-unknown-elf-

GCC = $(PREFIX)gcc
CC_COMPILE ?= $(GCC)
CC_LINK ?= $(GCC)
CXX = $(PREFIX)g++
CP = $(PREFIX)objcopy
OBJDUMP = $(PREFIX)objdump
DG = $(PREFIX)gdb
SIZE = $(PREFIX)size


#################################
# Flags
#################################

# SoC Settings
ARCH = rv64imafdc
ABI = lp64d
ARCHFLAGS = -march=$(ARCH) -mabi=$(ABI)

# Keep the libgloss reset entry first in .text.  Chipyard's boot ROM starts
# at 0x80000000; htif.ld places .text.init before application code.
CFLAGS  = -std=gnu99 -O2 -fno-common -fno-builtin-printf -Wall
CFLAGS += $(ARCHFLAGS)
# Reuse the upstream Gemmini software command definitions.  The dedicated
# test still emits custom3 instructions; ReRoCC routes that opcode to the
# acquired manager.
CFLAGS += -I../../gemmini/software/gemmini-rocc-tests
LDFLAGS = -static

include libgloss.mk

PROGRAMS = test gemmini_async_test rerocc_phase5_1r3c_test rerocc_phase5_1r1c3m_g8_test


.DEFAULT_GOAL := default

#################################
# Build
#################################

%.o: %.S
	$(CC_COMPILE) $(CFLAGS) -D__ASSEMBLY__=1 -c $< -o $@

%.o: %.c rerocc.h
	$(CC_COMPILE) $(CFLAGS) -c $< -o $@

%.riscv: %.o $(libgloss)
	$(CC_LINK) $(LDFLAGS) $< $(REROCC_LINK_LIBS) -o $@

%.dump: %.riscv
	$(OBJDUMP) -D $< > $@


#################################
# Recipes
#################################

.PHONY: default
default: $(addsuffix .dump, $(PROGRAMS)) $(addsuffix .riscv, $(PROGRAMS))

.PHONY: clean
clean:
	rm -f *.riscv *.o *.dump
	$(if $(libgloss),rm -rf $(libgloss_builddir)/)

.PHONY: dumps
dumps: $(addsuffix .dump, $(PROGRAMS))
