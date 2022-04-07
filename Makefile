TARGET = hp
CC ?= gcc
OBJS := main.o

CFLAGS = -Wall -Wextra -Wpedantic -O3 -std=c11 -fsanitize=thread

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f *.o $(TARGET)
