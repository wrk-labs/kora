include config.mk

VERSION = 0.1.0

SRC = src/main.c src/util.c src/model.c src/registry.c
OBJ = $(SRC:.c=.o)
BIN = kora

CFLAGS += -DVERSION=\"$(VERSION)\" -DLUADIR=\"$(LUADIR)\"

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(BIN) $(OBJ)

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	mkdir -p $(DESTDIR)$(LUADIR)/core
	mkdir -p $(DESTDIR)$(LUADIR)/plugins
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)
	cp -f lua/core/*.lua $(DESTDIR)$(LUADIR)/core/ 2>/dev/null || true
	cp -f lua/plugins/*.lua $(DESTDIR)$(LUADIR)/plugins/ 2>/dev/null || true

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -rf $(DESTDIR)$(LUADIR)

.PHONY: all clean install uninstall
