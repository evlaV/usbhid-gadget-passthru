all: steamdeck-hid-passthru

CFLAGS += -Wall -Wextra -Werror -Wno-format-truncation
.PHONY: clean

clean:
	rm steamdeck-hid-passthru steamdeck-hid-passthru.o

steamdeck-hid-passthru.o : steamdeck-hid-passthru.c
steamdeck-hid-passthru: steamdeck-hid-passthru.o
	$(CC) $(LDFLAGS) -o $@ $^
