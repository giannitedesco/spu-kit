MIN_CPU ?= native
TARGET_CPU ?= native

ARCH_CFLAGS := \
	-march=$(MIN_CPU) \
	-mtune=$(TARGET_CPU) \

TARGET_CFLAGS := \
	$(ARCH_CFLAGS)

#DEBUG_CFLAGS := -Og -fno-inline-functions

TARGET_DEFS := -D_GNU_SOURCE

ifdef BENCHMARK
BENCHMARK_DEFS := -DNDEBUG
endif

ifdef DEBUG
OPT_CFLAGS := -Og -fno-inline-functions
else
OPT_CFLAGS := \
	-O2 \
	-ggdb \
	-gdwarf-4 \
	-flto -fwhole-program -fno-fat-lto-objects \
	-finline-functions \
	-fgcse-after-reload -fgcse-las \
	-ftree-pre \
	-fipa-cp-clone \
	-fipa-pta
endif

WARN_CFLAGS := \
	-Wall \
	-Wextra \
	-Wno-unused-parameter \
	-Wmissing-declarations \
	-Wsuggest-attribute=pure \
	-Wsuggest-attribute=const \
	-Wsuggest-attribute=noreturn \
	-Wsuggest-attribute=format \
	-Wsuggest-attribute=cold \
	-Wsuggest-attribute=malloc\
	-Wmaybe-uninitialized \
	-Wlogical-op \
	-Wduplicated-cond \
	-Wduplicated-branches \
	-Wrestrict \
	-Wnull-dereference \
	-Wimplicit-fallthrough \
	-Wstrict-prototypes \
	-Wmissing-prototypes \
	-Wjump-misses-init \
	-Wswitch-enum

INCLUDES := -Iinclude

CC := gcc
CFLAGS := \
	$(TARGET_CFLAGS) \
	$(OPT_CFLAGS) \
	$(DEBUG_CFLAGS) \
	$(WARN_CFLAGS) \
	$(INCLUDES) \
	$(TARGET_DEFS) \
	$(BENCHMARK_DEFS)
