# ipsc2hbpc — IPSC to HomeBrew Protocol translator (C reimplementation)
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE
LDFLAGS ?=

SRC_DIR := src
DMR_DIR := src/dmr

SOURCES := \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/config.c \
	$(SRC_DIR)/toml.c \
	$(SRC_DIR)/log.c \
	$(SRC_DIR)/eventloop.c \
	$(SRC_DIR)/net.c \
	$(SRC_DIR)/crypto.c \
	$(SRC_DIR)/ipsc.c \
	$(SRC_DIR)/hbp.c \
	$(SRC_DIR)/translate.c \
	$(DMR_DIR)/dmr_bits.c \
	$(DMR_DIR)/dmr_const.c \
	$(DMR_DIR)/hamming.c \
	$(DMR_DIR)/crc.c \
	$(DMR_DIR)/rs129.c \
	$(DMR_DIR)/golay.c \
	$(DMR_DIR)/bptc.c \
	$(DMR_DIR)/ambe.c

OBJECTS := $(SOURCES:.c=.o)
BIN     := ipsc2hbpc

# DMR module sources (used by the standalone DSP self-test)
DMR_SOURCES := $(filter $(DMR_DIR)/%,$(SOURCES))

.PHONY: all clean test install uninstall

all: $(BIN)

# FHS install (run as root: sudo make install)
#   binary  -> $(PREFIX)/bin/ipsc2hbpc           (default /usr/local/bin)
#   config  -> $(SYSCONFDIR)/ipsc2hbp/           (default /etc/ipsc2hbp)
#   unit    -> $(UNITDIR)/ipsc2hbpc.service       (default /lib/systemd/system)
# The live config is NEVER overwritten: ipsc2hbp.toml is installed only if it
# does not already exist; the sample is always refreshed.  install does NOT
# enable or start the service.
PREFIX     ?= /usr/local
SYSCONFDIR ?= /etc
UNITDIR    ?= /lib/systemd/system
CONFDIR    := $(SYSCONFDIR)/ipsc2hbp
UNIT       := $(UNITDIR)/ipsc2hbpc.service

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/ipsc2hbpc
	install -d $(DESTDIR)$(CONFDIR)
	install -m 0644 ipsc2hbp.toml.sample $(DESTDIR)$(CONFDIR)/ipsc2hbp.toml.sample
	@if [ -f $(DESTDIR)$(CONFDIR)/ipsc2hbp.toml ]; then \
	    echo "Preserving existing $(DESTDIR)$(CONFDIR)/ipsc2hbp.toml (not overwritten)"; \
	else \
	    install -m 0644 ipsc2hbp.toml.sample $(DESTDIR)$(CONFDIR)/ipsc2hbp.toml; \
	    echo "Installed fresh $(DESTDIR)$(CONFDIR)/ipsc2hbp.toml — edit it before starting"; \
	fi
	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 ipsc2hbpc.service $(DESTDIR)$(UNIT)
	-systemctl daemon-reload
	@echo
	@echo "Installed. NOT enabled/started. If the Python bridge is running, stop it first:"
	@echo "    sudo systemctl stop ipsc2hbp && sudo systemctl disable ipsc2hbp"
	@echo "Then edit $(CONFDIR)/ipsc2hbp.toml and:"
	@echo "    sudo systemctl enable --now ipsc2hbpc"

# Remove binary and unit; the config directory is left in place on purpose.
uninstall:
	-systemctl disable --now ipsc2hbpc
	-rm -f $(DESTDIR)$(PREFIX)/bin/ipsc2hbpc
	-rm -f $(DESTDIR)$(UNIT)
	-systemctl daemon-reload
	@echo "Removed binary and unit. Left $(CONFDIR) intact (delete manually if desired)."

$(BIN): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Translator/DSP sources used by the test harnesses (no ipsc/hbp/main)
TEST_SUPPORT := $(SRC_DIR)/config.c $(SRC_DIR)/toml.c $(SRC_DIR)/log.c \
	$(SRC_DIR)/eventloop.c $(SRC_DIR)/translate.c $(DMR_SOURCES)

# Tests:
#  - test_dsp:    dmr module vs golden vectors generated from dmr_utils3
#  - test_parity: C translator output vs Python translator output, byte-for-byte
test: tests/test_dsp.c tests/test_parity.c $(TEST_SUPPORT)
	$(CC) $(CFLAGS) -I. -o /tmp/ipsc2hbpc_test_dsp tests/test_dsp.c $(DMR_SOURCES)
	/tmp/ipsc2hbpc_test_dsp tests/dsp_vectors.txt
	$(CC) $(CFLAGS) -I. -o /tmp/ipsc2hbpc_test_parity tests/test_parity.c $(TEST_SUPPORT)
	/tmp/ipsc2hbpc_test_parity

clean:
	rm -f $(OBJECTS) $(BIN)
