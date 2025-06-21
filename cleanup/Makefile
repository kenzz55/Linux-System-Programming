CC = gcc
CFLAGS = -Wall -g
TARGET = ssu_cleanup
OBJS = ssu_cleanup.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

ssu_cleanup.o: ssu_cleanup.c
	$(CC) $(CFLAGS) -c $^

clean:
	rm -f $(OBJS) $(TARGET)
