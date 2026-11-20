PREFIX   ?= /usr/local
CC       ?= cc
CFLAGS   ?= -Os -Wall -Wextra
FAAD     ?= $(shell brew --prefix faad2 2>/dev/null || echo /usr/local)

BUILD    := build
BEARSSL  := vendor/BearSSL
BRLIB    := $(BUILD)/bearssl/libbearssl.a
SRCS     := src/main.c src/net.c src/yt.c src/audio.c src/decode.c src/resample.c
OBJS     := $(SRCS:src/%.c=$(BUILD)/%.o)

CPPFLAGS += -I$(BEARSSL)/inc -I$(FAAD)/include
LDLIBS   += $(BRLIB) -L$(FAAD)/lib -lfaad -framework CoreAudio -framework CoreFoundation

all: claude-radio

claude-radio: $(OBJS) $(BRLIB)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: src/%.c src/*.h $(BRLIB) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

$(BEARSSL)/inc/bearssl.h:
	git submodule update --init $(BEARSSL)

$(BRLIB): $(BEARSSL)/inc/bearssl.h
	$(MAKE) -C $(BEARSSL) BUILD=$(abspath $(BUILD)/bearssl) CFLAGS="-W -Wall -Os -fPIC" lib

bearssl-tools: $(BEARSSL)/inc/bearssl.h
	$(MAKE) -C $(BEARSSL) BUILD=$(abspath $(BUILD)/bearssl) CFLAGS="-W -Wall -Os -fPIC" lib tools

install: claude-radio
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 claude-radio $(DESTDIR)$(PREFIX)/bin/claude-radio

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/claude-radio

clean:
	rm -rf $(BUILD) claude-radio

.PHONY: all install uninstall clean bearssl-tools
