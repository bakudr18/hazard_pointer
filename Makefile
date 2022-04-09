TARGET = hp
CC ?= gcc
OBJS := main.o

CFLAGS = -g -Wall -Wextra -Wpedantic -O1 -std=c11 -fsanitize=thread

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f *.o $(TARGET)
