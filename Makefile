all: usbhid-gadget-passthru

CFLAGS += -Wall -Wextra -Werror -Wno-format-truncation -Wno-stringop-overflow

ifeq ($(DEBUG),)
  CFLAGS += -O2
else
  CFLAGS += -g
endif

.PHONY: clean install

clean:
	rm usbhid-gadget-passthru main.o

install: all
	install -Ds -m755 -t "$(DESTDIR)/usr/bin" usbhid-gadget-passthru

main.o: main.c
usbhid-gadget-passthru: main.o
	$(CC) $(LDFLAGS) -o $@ $^
