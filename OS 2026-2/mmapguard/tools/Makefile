CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -g

.PHONY: all clean

all: mg_encrypt mg_test mg_bench

mg_encrypt: mg_encrypt.c
	$(CC) $(CFLAGS) -o $@ $< -lcrypto

mg_test: mg_test.c ../src/mmapguard_uapi.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f mg_encrypt mg_test mg_bench

mg_bench: mg_bench.c ../src/mmapguard_uapi.h
	$(CC) $(CFLAGS) -o $@ $<
