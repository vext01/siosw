LDFLAGS += -lsndio -lcurses -lmenu
CFLAGS += -Wall -Wextra -pedantic

all: siosw

sndiosw: siosw.c

clean:
	-rm -f siosw
