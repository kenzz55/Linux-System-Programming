CC = gcc
CFLAGS = -Wall -g
TARGET = ssu_ext2
OBJS = ssu_ext2.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

ssu_ext2.o: ssu_ext2.c
	$(CC) $(CFLAGS) -c $^

clean:
	rm -f $(OBJS) $(TARGET)
