CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LIBUSB_PREFIX ?= $(shell brew --prefix libusb)
CPPFLAGS += -I$(LIBUSB_PREFIX)/include/libusb-1.0
LDFLAGS += -L$(LIBUSB_PREFIX)/lib
LDLIBS += -lusb-1.0

.PHONY: all test clean

all: athena_diag_uc12

athena_diag_uc12: athena_diag_uc12.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

test: athena_diag_uc12
	./athena_diag_uc12 --self-test

clean:
	rm -f athena_diag_uc12
