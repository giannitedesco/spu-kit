include mk/header.mk
include mk/compiler.mk

SRC_DIR := src
OBJ_DIR := obj
BIN_DIR := bin

include mk/rules.mk

SPUKIT_SRC := \
	fd.c \
	bufwr.c \
	hexdump.c \
	system.c \
	spc700.c \
	apu.c \
	dsp.c \
	wav.c \
	main.c

$(eval $(call make_bin,spukit,$(SPUKIT_SRC)))

include mk/targets.mk
include mk/deps.mk
