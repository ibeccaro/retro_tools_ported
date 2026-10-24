CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-format-truncation

apridisk: apridisk.c
	$(CC) $(CFLAGS) -o $@ $<

test: apridisk
	sh ./test.sh

clean:
	rm -f apridisk
