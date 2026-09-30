CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE -pthread
LDFLAGS = -pthread
TARGET = sys_shell
SRCS = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c cat_huffman.c huffman.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

%.o: %.c shell.h huffman.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
	rm -rf pruebas_tmp

.PHONY: all clean
