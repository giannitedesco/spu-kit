MIN_CPU ?= native
TARGET_CPU ?= native

ARCH_CFLAGS := \
	-march=$(MIN_CPU) \
	-mtune=$(TARGET_CPU) \

TARGET_CFLAGS := \
	$(ARCH_CFLAGS)

DEBUG_CFLAGS := \
	-ggdb \
	-gdwarf-4

TARGET_DEFS := -D_GNU_SOURCE

ifdef SANITIZE
SANITIZE_CFLAGS := -fsanitize=undefined,address -fno-sanitize=alignment
endif

ifdef BENCHMARK
BENCHMARK_DEFS := -DNDEBUG
endif

ifdef ANALYZER
ANALYZER_CFLAGS := -fanalyzer
endif

ifdef DEBUG
OPT_CFLAGS := -Og -fno-inline-functions
else
OPT_CFLAGS := \
	-O2 \
	-flto -fwhole-program -fno-fat-lto-objects \
	-finline-functions \
	-ftree-partial-pre \
	-fgcse-after-reload \
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
	$(SANITIZE_CFLAGS) \
	$(ANALYZER_CFLAGS) \
	$(DEBUG_CFLAGS) \
	$(WARN_CFLAGS) \
	$(INCLUDES) \
	$(TARGET_DEFS) \
	$(BENCHMARK_DEFS)
