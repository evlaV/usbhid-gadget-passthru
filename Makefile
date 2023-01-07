all: usbhid-gadget-passthru usbhid-bt-passthru

BLUEZ_CFLAGS := $(shell pkg-config --cflags bluez)
BLUEZ_LDFLAGS := $(shell pkg-config --libs bluez)
SDBUS_CFLAGS := $(shell pkg-config --cflags libsystemd)
SDBUS_LDFLAGS := $(shell pkg-config --libs libsystemd)

CFLAGS += -Wall -Wextra -Werror -Wno-format-truncation -Wno-stringop-overflow -Iinclude

ifneq ($(ASAN),)
  CFLAGS += -g -fsanitize=address
  LDFLAGS += -g -fsanitize=address
else ifneq ($(DEBUG),)
  CFLAGS += -g
  LDFLAGS += -g
else
  CFLAGS += -O2 -D_FORTIFY_SOURCE=2
endif

OBJS=\
	src/dev.o \
	src/log.o \
	src/options.o \
	src/usb.o \
	src/util.o

.PHONY: clean install

clean:
	rm -f usbhid-gadget-passthru usbhid-bt-passthru $(OBJS) src/bt.o src/dbus.o src/gatt.o src/udc.o

install: all
	install -Ds -m755 -t "$(DESTDIR)/usr/bin" usbhid-gadget-passthru
	install -Ds -m755 -t "$(DESTDIR)/usr/bin" usbhid-bt-passthru

src/dev.o: include/dev.h include/log.h include/util.h
src/options.o: include/options.h
src/usb.o: include/usb.h include/dev.h include/log.h include/util.h
src/util.o: include/util.h include/log.h

src/bt.o: include/dbus.h include/dev.h include/gatt.h include/usb.h include/util.h
src/dbus.o: include/dbus.h
src/gatt.o: include/dbus.h include/gatt.h
src/udc.o: include/dev.h include/log.h include/options.h include/usb.h include/util.h

include/gatt.h: include/dbus.h

src/bt.o: CFLAGS += $(BLUEZ_CFLAGS) $(SDBUS_CFLAGS)
src/dbus.o src/gatt.o: CFLAGS += $(SDBUS_CFLAGS)

usbhid-gadget-passthru: $(OBJS) src/udc.o
	$(CC) $(LDFLAGS) -o $@ $^

usbhid-bt-passthru: $(OBJS) src/bt.o src/dbus.o src/gatt.o
	$(CC) $(LDFLAGS) $(BLUEZ_LDFLAGS) $(SDBUS_LDFLAGS) -o $@ $^
