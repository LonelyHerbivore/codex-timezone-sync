CC ?= cc
PYTHON ?= python3
CFLAGS := -O2 -std=c11 -Wall -Wextra -Werror

ifeq ($(OS),Windows_NT)
LIB := codex-timezone-sync.dll
LDFLAGS := -shared -static-libgcc -lws2_32
else
CFLAGS += -fPIC -fstack-protector-strong
ifeq ($(shell uname -s),Darwin)
LIB := codex-timezone-sync.dylib
LDFLAGS := -dynamiclib -pthread
else
LIB := codex-timezone-sync.so
LDFLAGS := -pthread -shared
endif
endif

all: $(LIB)
dashboard_bundle.h: dashboard.html embed_ui.py
	$(PYTHON) embed_ui.py
$(LIB): plugin.c settings.inc events.inc stats.inc refresh_bridge.inc failures.inc timezone_normalize.inc model_monitor.inc platform_compat.h dashboard_bundle.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
test: all
	$(PYTHON) test_plugin.py
clean:
	rm -f codex-timezone-sync.so codex-timezone-sync.dylib codex-timezone-sync.dll dashboard_bundle.h
.PHONY: all clean test

# Optional browser tests require Playwright and Chromium.
test-ui:
	$(PYTHON) test_ui.py
.PHONY: test-ui
