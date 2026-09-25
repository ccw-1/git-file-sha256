CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
TARGET = git-file-sha256

all: $(TARGET)

$(TARGET): git-file-sha256.c
	$(CC) $(CFLAGS) -o $(TARGET) git-file-sha256.c

clean:
	rm -f $(TARGET)
