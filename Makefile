CC = gcc
CFLAGS = -O0 -Wall -Wextra -Werror -o decoder -lpthread
LDFLAGS = -lm

TARGET = decode
SRCS = main.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
