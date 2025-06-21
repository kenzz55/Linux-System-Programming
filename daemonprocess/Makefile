CC = gcc
CFLAGS = -Wall -g
TARGET = ssu_cleanupd
OBJS = ssu_cleanupd.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

ssu_cleanupd.o: ssu_cleanupd.c
	$(CC) $(CFLAGS) -c $^

clean:
	rm -f $(OBJS) $(TARGET)
