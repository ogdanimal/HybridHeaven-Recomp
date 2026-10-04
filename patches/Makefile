# Builds patches.elf -- MIPS objects that N64Recomp then recompiles in
# single-file output mode, producing C++ that overrides the recompiler's own
# version of the patched functions.
#
# Adapted from Goemon64Recomp/patches/Makefile.

# Output definitions
TARGET_NAME := patches.elf
BUILD_DIR   := build
TARGET      := $(BUILD_DIR)/$(TARGET_NAME)

# Tools
# These must be ':=' not '?=': GNU make predefines CC=cc AND LD=ld, so '?='
# would never fire and a bare `make -C patches` picks up the host toolchain.
# With the host ld that fails loudly ("relocations in generic ELF (EM: 8)"),
# but with the host cc it fails as a wall of cryptic errors instead. A
# command-line `CC=...` still overrides ':='.
#
# (Goemon64Recomp's Makefile uses '?=' for LD and claims make does not
# predefine it. It does -- check `make -p | grep '^LD'`.)
CC      := clang
LD      := ld.lld
NM      ?= llvm-nm
READELF ?= llvm-readelf
CMAKE   ?= cmake

# Flags
CFLAGS   := -target mips -mips2 -mabi=32 -O2 -G0 \
            -mno-abicalls -mno-odd-spreg -mno-check-zero-division \
            -fomit-frame-pointer -ffast-math -fno-unsafe-math-optimizations \
            -fno-builtin-memset -Wall -Wextra \
            -Wno-incompatible-library-redeclaration -Wno-unused-parameter \
            -Wno-unknown-pragmas -Wno-unused-variable \
            -Wno-missing-braces -Wno-unsupported-floating-point-opt

CPPFLAGS := -nostdinc -D_LANGUAGE_C -DMIPS -DNDEBUG \
            -I ../lib/hybridheaven/include \
            -I ../lib/hybridheaven/libultra/include \
            -I ../lib/hybridheaven/libultra/include/PR \
            -I ../lib/rt64/include

ASFLAGS  := -target mips -mips2 -mabi=32 -G0 \
            -mno-abicalls -mno-check-zero-division \
            -x assembler-with-cpp -modd-spreg

LDSCRIPTS := patches.ld syms.ld
LDFLAGS  := -nostdlib $(addprefix -T ,$(LDSCRIPTS)) \
            -Map $(BUILD_DIR)/patches.map \
            --unresolved-symbols=ignore-all --emit-relocs

# Source processing
C_SRCS := $(wildcard *.c)
C_OBJS := $(C_SRCS:%.c=$(BUILD_DIR)/%.o)
C_DEPS := $(C_OBJS:.o=.d)

# Assembly processing
S_SRCS := $(wildcard *.s)
S_OBJS := $(addprefix $(BUILD_DIR)/, $(S_SRCS:.s=.o))

# Default target
all: $(TARGET)

# Linker Rule
$(TARGET): $(C_OBJS) $(S_OBJS) $(LDSCRIPTS)
	@$(CMAKE) -E echo "Linking $@"
	@$(CMAKE) -E make_directory $(BUILD_DIR)
	$(LD) $(C_OBJS) $(S_OBJS) $(LDFLAGS) -o $@
	@# --unresolved-symbols=ignore-all lets a mistyped host export (one referenced
	@# from patch C but missing from syms.ld) link as address 0 instead of failing,
	@# so it silently becomes a call into the wrong host function at runtime. Every
	@# recomp_* shim is DEFINED in syms.ld, so an *undefined* recomp_* symbol here
	@# is unambiguously that typo. Game symbols (func_/D_/...) and libultra symbols
	@# are resolved by name downstream by N64Recomp and must stay undefined, so they
	@# are deliberately not flagged.
	@bad=`$(NM) $(TARGET) | awk '$$1=="U"||$$2=="U"{print $$NF}' | grep '^recomp_' || true`; \
	if [ -n "$$bad" ]; then \
		$(CMAKE) -E echo "ERROR: undefined recomp_* host export(s) — add to syms.ld:"; \
		for s in $$bad; do $(CMAKE) -E echo "  $$s"; done; \
		exit 1; \
	fi
	@# The other way an undefined symbol gets through, which the check above
	@# cannot see. A patch that reaches game DATA through an undefined symbol
	@# links fine and faults at runtime: N64Recomp resolves it by name and emits
	@# RELOC_HI16/RELOC_LO16 against the *reference* section index space, while
	@# the generated code indexes the one `section_addresses` array the base
	@# recompilation fills in using ITS index space. Those two disagree here --
	@# .main is 6 in one and 1 in the other, see the header of syms.ld -- so the
	@# lookup reads 0 and the patch dereferences a small offset. That is exactly
	@# what made the loader hook segfault the first time it ran.
	@#
	@# Undefined FUNCTION symbols are fine and must stay that way: a direct call
	@# is R_MIPS_26, which carries no section index, and N64Recomp binds it by
	@# name downstream. Only HI16/LO16 carry one. So the bug has a precise
	@# signature -- an undefined symbol reached by a data reloc -- and that is
	@# what this looks for, rather than flagging every undefined symbol.
	@undef=`$(NM) $(TARGET) | awk '$$1=="U"||$$2=="U"{print $$NF}'`; \
	bad=`$(READELF) -r $(TARGET) | awk -v u="$$undef" \
		'BEGIN{n=split(u,a," "); for(i=1;i<=n;i++) U[a[i]]=1} \
		 /R_MIPS_(HI16|LO16)/ && ($$NF in U) {print $$NF}' | sort -u`; \
	if [ -n "$$bad" ]; then \
		$(CMAKE) -E echo "ERROR: undefined symbol(s) reached by a data reloc — define as absolute in syms.ld:"; \
		for s in $$bad; do $(CMAKE) -E echo "  $$s"; done; \
		exit 1; \
	fi

# Compiler Rule
$(BUILD_DIR)/%.o: %.c
	@$(CMAKE) -E echo "Compiling $<"
	@$(CMAKE) -E make_directory $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -MMD -MF $(@:.o=.d) -c -o $@

$(BUILD_DIR)/%.o: %.s
	@$(CMAKE) -E echo "Compiling $<"
	@$(CMAKE) -E make_directory $(BUILD_DIR)
	$(CC) $(ASFLAGS) $(CPPFLAGS) $< -MMD -MF $(@:.o=.d) -c -o $@

clean:
	@$(CMAKE) -E echo "Cleaning build directory..."
	$(CMAKE) -E remove_directory $(BUILD_DIR)

-include $(C_DEPS)

.PHONY: all clean
