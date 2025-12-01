LDFLAGS += -lsndio -lcurses -lmenu
CFLAGS += -Wall -Wextra -pedantic

all: siosw

sndiosw: siosw.c

.PHONY: clean
clean:
	-rm -f siosw
