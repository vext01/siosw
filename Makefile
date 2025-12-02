LDFLAGS += -lsndio -lcurses -lmenu
CFLAGS += -Wall -Wextra -pedantic
PREFIX ?= /usr/local

all: siosw

sndiosw: siosw.c

install: siosw
	install -d ${PREFIX}/bin
	install siosw ${PREFIX}/bin/

.PHONY: clean
clean:
	-rm -f siosw
