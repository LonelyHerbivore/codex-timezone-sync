CC ?= cc
CFLAGS := -O2 -std=c11 -fPIC -Wall -Wextra -Werror -fstack-protector-strong
all: codex-timezone-sync.so
dashboard_bundle.h: dashboard.html embed_ui.py
	python3 embed_ui.py
codex-timezone-sync.so: plugin.c settings.inc events.inc stats.inc refresh_bridge.inc failures.inc timezone_normalize.inc model_monitor.inc dashboard_bundle.h
	$(CC) $(CFLAGS) -pthread -shared -o $@ $<
test: all
	python3 test_plugin.py
clean:
	rm -f codex-timezone-sync.so dashboard_bundle.h
.PHONY: all clean test

# Optional browser tests require Playwright and Chromium.
test-ui:
	python3 test_ui.py
.PHONY: test-ui
