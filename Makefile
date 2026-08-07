PREFIX ?= $(HOME)/.local
BIN_DIR ?= $(PREFIX)/bin
BUILD_DIR ?= build
MESON ?= meson
NINJA ?= ninja
MESON_ARGS ?= -Dxephyr=true -Dxorg=false -Dxnest=false -Dxvfb=false -Dxwin=false -Dxquartz=false -Ddocs=false -Ddevel-docs=false -Ddocs-pdf=false
XEPHYR_BIN := $(BUILD_DIR)/hw/kdrive/ephyr/Xephyr

.DEFAULT_GOAL := build

.PHONY: setup reconfigure build install reinstall uninstall clean distclean info test-relative-pointer

setup:
	$(MESON) setup $(BUILD_DIR) $(MESON_ARGS)

reconfigure:
	@if [ ! -d $(BUILD_DIR) ]; then \
		$(MESON) setup $(BUILD_DIR) $(MESON_ARGS); \
	else \
		$(MESON) setup --reconfigure $(BUILD_DIR) $(MESON_ARGS); \
	fi

build:
	@if [ ! -f $(BUILD_DIR)/build.ninja ]; then \
		$(MESON) setup $(BUILD_DIR) $(MESON_ARGS); \
	fi
	$(NINJA) -C $(BUILD_DIR) hw/kdrive/ephyr/Xephyr

install: build
	install -d $(BIN_DIR)
	install -m755 $(XEPHYR_BIN) $(BIN_DIR)/Xephyr

reinstall: install

uninstall:
	rm -f $(BIN_DIR)/Xephyr

clean:
	@if [ -d $(BUILD_DIR) ]; then $(NINJA) -C $(BUILD_DIR) -t clean; fi

distclean:
	rm -rf $(BUILD_DIR)

info: build
	@echo "Built binary: $(abspath $(XEPHYR_BIN))"
	@echo "Install path: $(BIN_DIR)/Xephyr"
	@strings $(XEPHYR_BIN) | grep -F 'ctrl+shift+space' || true

test-relative-pointer: build
	XEPHYR_BIN="$(abspath $(XEPHYR_BIN))" ./test/ephyr/test-relative-pointer.sh
