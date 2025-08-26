CC      := cc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wformat=2 -fno-common -D_GNU_SOURCE
LDFLAGS := 
LIBS    := 

SRC := dpmd.c jsmn.c
OBJ := $(SRC:.c=.o)

BIN := dpmd

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

%.o: %.c dpmd.h jsmn.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)
