CC      = gcc
CFLAGS  = -Wall -Wextra -std=gnu11 -g
SRC     = src/main.c src/parser.c src/jobs.c src/builtins.c src/exec.c
OBJ     = $(SRC:.c=.o)
TARGET  = tinyshell

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

%.o: %.c src/shell.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
