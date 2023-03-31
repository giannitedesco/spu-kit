.PHONY: all
all: all_bin

.PHONY: all_bin
all_bin: $(ALL_BIN)

.PHONY: clean
clean:
	$(info clean)
	rm -rf $(OBJ_DIR) $(BIN_DIR) $(EXTRA_CLEAN) $(ALL_GEN)
	find . -regex '^.*\(__pycache__\|\.py[co]\)$$' -delete

.PRECIOUS: $(ALL_GEN)
