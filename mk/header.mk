MAKEFLAGS += --no-builtin-rules
.SUFFIXES:
.SECONDEXPANSION:

ifdef V
MAKEFLAGS += --verbose
else
MAKEFLAGS += --quiet
endif

TARGET: all
