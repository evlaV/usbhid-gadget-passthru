all: usbhid-gadget-passthru

CFLAGS += -Wall -Wextra -Werror -Wno-format-truncation -Wno-stringop-overflow -Iinclude

ifeq ($(DEBUG),)
  CFLAGS += -O2 -D_FORTIFY_SOURCE=2
else
  CFLAGS += -g
endif

OBJS=\
	src/dev.o \
	src/main.o \
	src/options.o \
	src/usb.o \
	src/util.o

.PHONY: clean install

clean:
	rm usbhid-gadget-passthru $(OBJS)

install: all
	install -Ds -m755 -t "$(DESTDIR)/usr/bin" usbhid-gadget-passthru

src/dev.o: include/util.h
src/main.o: include/dev.h include/options.h include/usb.h include/util.h
src/options.o: include/options.h
src/usb.o: include/dev.h include/util.h
src/util.o: include/util.h

usbhid-gadget-passthru: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
