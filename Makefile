PREFIX  ?= $(HOME)/.local
BIN      = iss
LABEL    = com.instant-swipe
AGENT_DIR = $(HOME)/Library/LaunchAgents
PLIST     = $(AGENT_DIR)/$(LABEL).plist

# Xcode 26.x's linker (TAPI 21.0.0) cannot parse the arm64e.x1 slices in the
# newer CommandLineTools MacOSX27 SDK, so pin to the newest SDK the active
# toolchain understands. Re-check after updating Xcode: if the default SDK's
# .tbd files link cleanly, this pin can be dropped.
SDKROOT ?= $(shell xcrun --sdk macosx26.5 --show-sdk-path 2>/dev/null || xcrun --sdk macosx --show-sdk-path 2>/dev/null)
CFLAGS  ?= -std=c11 -O3 -march=native -pipe -flto -Wall -Wextra -Wpedantic
ifneq ($(SDKROOT),)
CFLAGS  += -isysroot $(SDKROOT)
endif
LDFLAGS  = -framework ApplicationServices -framework CoreFoundation \
           -Wl,-dead_strip -Wl,-dead_strip_dylibs -Wl,-x

$(BIN): iss.c Makefile
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

lint: iss.c
	cppcheck --enable=warning,style,performance --error-exitcode=1 iss.c

test: iss.c
	$(CC) $(CFLAGS) -Werror -fsyntax-only iss.c
	mkdir -p .work/tmp
	$(CC) $(CFLAGS) -Werror -o .work/tmp/iss-test iss.c $(LDFLAGS)
	@set -eu; \
	for option in -h --help; do \
	  if .work/tmp/iss-test "$$option" > .work/tmp/iss-help.out 2> .work/tmp/iss-help.err; then :; else exit 1; fi; \
	  test ! -s .work/tmp/iss-help.err; \
	  grep -Fqx 'Maintained by Joshua Li: https://github.com/joshuarli/iss' .work/tmp/iss-help.out; \
	done

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)
	codesign -fs - $(PREFIX)/bin/$(BIN)
	xattr -d com.apple.quarantine $(PREFIX)/bin/$(BIN) 2>/dev/null || true
	mkdir -p $(AGENT_DIR)
	printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"' \
	  '  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0">' \
	  '<dict>' \
	  '  <key>Label</key>' \
	  '  <string>$(LABEL)</string>' \
	  '  <key>ProgramArguments</key>' \
	  '  <array>' \
	  '    <string>$(PREFIX)/bin/$(BIN)</string>' \
	  '  </array>' \
	  '  <key>RunAtLoad</key>' \
	  '  <true/>' \
	  '  <key>KeepAlive</key>' \
	  '  <true/>' \
	  '</dict>' \
	  '</plist>' > $(PLIST)
	launchctl bootout gui/$$(id -u) $(PLIST) 2>/dev/null || true
	launchctl bootstrap gui/$$(id -u) $(PLIST)

uninstall:
	launchctl bootout gui/$$(id -u) $(PLIST) 2>/dev/null || true
	rm -f $(PREFIX)/bin/$(BIN) $(PLIST)

clean:
	rm -f $(BIN)

.PHONY: install uninstall clean lint test
