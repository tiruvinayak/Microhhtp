CC = gcc

WARNINGS = -Wall -Wextra -Wpedantic
CFLAGS = -std=c11 $(WARNINGS) -O2 -pthread
LDFLAGS = -pthread

TARGET      = build/microhttp
CLIENT      = build/client

SRC = src/main.c src/parser.c src/files.c src/server.c src/threadpool.c
HDR = src/parser.h src/files.h src/server.h src/threadpool.h

all: $(TARGET) $(CLIENT)

$(TARGET): $(SRC) $(HDR)
	mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

$(CLIENT): client.c
	mkdir -p build
	$(CC) $(CFLAGS) client.c -o $@ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

# Debug build: no optimization, full symbols (gdb / lldb friendly).
debug: CFLAGS = -std=c11 $(WARNINGS) -O0 -g3 -pthread
debug: clean all

# ASan + UBSan build. Run the suite under it:  make sanitize && make test
# (On some systems add -fno-omit-frame-pointer for better traces.)
sanitize: CFLAGS = -std=c11 $(WARNINGS) -O1 -g -pthread \
	-fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: LDFLAGS = -pthread -fsanitize=address,undefined
sanitize: clean all

test: all
	bash tests/test_http.sh
	bash tests/test_client.sh

clean:
	rm -rf build/*

.PHONY: all run test clean debug sanitize
