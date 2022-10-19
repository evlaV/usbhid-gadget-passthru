all: steamdeck-hid-passthru

CFLAGS += -Wall -Wextra -Werror -Wno-format-truncation -Wno-stringop-overflow

ifeq ($(DEBUG),)
  CFLAGS += -O2
else
  CFLAGS += -g
endif

.PHONY: clean install

clean:
	rm steamdeck-hid-passthru steamdeck-hid-passthru.o

install: all
	install -Ds -m755 -t "$(DESTDIR)/usr/bin" steamdeck-hid-passthru

steamdeck-hid-passthru.o : steamdeck-hid-passthru.c
steamdeck-hid-passthru: steamdeck-hid-passthru.o
	$(CC) $(LDFLAGS) -o $@ $^
