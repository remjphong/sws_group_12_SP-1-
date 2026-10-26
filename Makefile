CC = gcc
CPPFLAGS = -Isrc -D_GNU_SOURCE
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2
TARGET = sws
SRC = src/main.c src/server.c src/http.c src/files.c src/cgi.c
OBJ = $(SRC:.c=.o)

.PHONY: all clean test
all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	bash tests/test_http.sh
	bash tests/test_files.sh
	bash tests/test_cgi.sh

clean:
	rm -f $(OBJ) $(TARGET)
