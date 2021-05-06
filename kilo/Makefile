BIN = kilo
CC = GCC
CFLAGS = -Wall -Wextra -pedantic -std=c99
RM = rm -f

.PHONY: all
all: $(BIN)

$(BIN): kilo.c
	$(CC) $(CFLAGS) -o $@ $^ 

.PHONY: clean
clean:
	$(RM) $(BIN) *.o
