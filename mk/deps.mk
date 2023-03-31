ALL_DEP := $(call depfile,$(ALL_OBJ)) $(EXTRA_DEPS)

ifneq ($(filter clean, $(MAKECMDGOALS)), clean)
-include $(ALL_DEP)
endif
