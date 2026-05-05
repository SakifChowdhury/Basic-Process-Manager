CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -g -std=c11 -D_GNU_SOURCE
LDFLAGS = -lpthread
TARGET  = process_manager
SRCS    = process_manager.c process_table.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build successful → ./$(TARGET)"

%.o: %.c process_table.h
	$(CC) $(CFLAGS) -c -o $@ $<

run: all
	./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Cleaned."
