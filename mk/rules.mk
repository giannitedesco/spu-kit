objfile = $(addsuffix .o,$(basename $(addprefix $(OBJ_DIR)/,$1)))
srcfile = $(addprefix $(SRC_DIR)/,$1)
depfile = $(addsuffix .d,$(basename $1))

STAMP_NAME := .stamp

dstamp = $1/$(STAMP_NAME)

.PRECIOUS: %/$(STAMP_NAME)

%/$(STAMP_NAME):
	@mkdir -p $(dir $@)
	@touch $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(MAKEFILE_LIST) | $(call dstamp,$$(@D))
	$(info compile: $(patsubst $(OBJ_DIR)/%,%,$@))
	$(CC) $(CFLAGS) -MMD -MP -MF $(call depfile,$@) -MT $@ -c -o $@ $<

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cc $(MAKEFILE_LIST) | $(call dstamp,$$(@D))
	$(info c-minus: $(patsubst $(OBJ_DIR)/%,%,$@))
	$(CPP) $(CPPFLAGS) -MMD -MP -MF $(call depfile,$@) -MT $@ -c -o $@ $<

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.S $(MAKEFILE_LIST) | $(call dstamp,$$(@D))
	$(info assemble: $(patsubst $(OBJ_DIR)/%,%,$@))
	$(CC) $(CFLAGS) -D__ASM__ -MMD -MP -MF $(call depfile,$@) -MT $@ \
		-c -o $@ $<

ALL_OBJ =
ALL_BIN =
ALL_GEN =

# Macro _make_bin
#
# Creates a rule to build a binary from a list of sources. Also creates a
# target such named for the binary, ie. so that bin/foo gets a convenience
# target called foo.
#
# Object paths have already been expanded by make_bin macro
#
# 1 bin-name
# 2 src-list
# 3 obj-list
# 4 libs
# 5 compiler
define _make_bin
ALL_OBJ += $3
ALL_BIN += $(BIN_DIR)/$1

$(BIN_DIR)/$1: $$(MAKEFILE_LIST) $3 | $(call dstamp,$(BIN_DIR))
	$$(info link: $1)
	$$($5) $$(CFLAGS) -o $$@ $3 $4

.PHONY: $1
$1: $(BIN_DIR)/$1

endef

# 1 bin-name
# 2 src-list
# 3 (optional) libs
define make_bin
$(call _make_bin,$1,$2,$(call objfile,$2),$3,CC)
endef

define make_cpp_bin
$(call _make_bin,$1,$2,$(call objfile,$2),$3,CPP)
endef


define _add_cflags
$1: CFLAGS += $2
endef

# 1 src-list
# 2 cflags
define add_cflags
$(call _add_cflags,$(call objfile,$1),$2)
endef
