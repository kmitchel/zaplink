CC = gcc
CFLAGS ?= -O2 -g -Wall -Wextra
CPPFLAGS ?= -I./include -I./deps/include
LDFLAGS = -L./deps/lib -lsqlite3 -lpthread

SRC_DIR = src
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

INSTALL_DIR = /opt/zaplink
STATE_DIR = /var/lib/zaplink
SERVICE_DIR = /etc/systemd/system

SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/channels.c $(SRC_DIR)/db.c $(SRC_DIR)/epg.c \
       $(SRC_DIR)/huffman.c $(SRC_DIR)/scanner.c $(SRC_DIR)/tuner.c \
       $(SRC_DIR)/stream_config.c $(SRC_DIR)/stream_session.c \
       $(SRC_DIR)/transcode.c $(SRC_DIR)/http_server.c $(SRC_DIR)/benchmark.c \
       $(SRC_DIR)/thread_pool.c

OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

TARGET = $(BUILD_DIR)/zaplink
TEST_TARGET = $(BUILD_DIR)/test_channels
TEST_TARGETS = $(TEST_TARGET) $(BUILD_DIR)/test_tuner $(BUILD_DIR)/test_db_concurrency \
               $(BUILD_DIR)/test_stream_config $(BUILD_DIR)/test_stream_session \
               $(BUILD_DIR)/test_transcode $(BUILD_DIR)/test_huffman

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Build complete: $@"


$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -rf $(BUILD_DIR)

$(TEST_TARGET): tests/test_channels.c $(OBJ_DIR)/channels.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_tuner: tests/test_tuner.c $(OBJ_DIR)/tuner.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_db_concurrency: tests/test_db_concurrency.c $(OBJ_DIR)/db.o $(OBJ_DIR)/channels.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_stream_config: tests/test_stream_config.c $(OBJ_DIR)/stream_config.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_stream_session: tests/test_stream_session.c $(OBJ_DIR)/stream_session.o $(OBJ_DIR)/stream_config.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_transcode: tests/test_transcode.c $(OBJ_DIR)/transcode.o $(OBJ_DIR)/stream_config.o $(OBJ_DIR)/tuner.o $(OBJ_DIR)/channels.o $(OBJ_DIR)/stream_session.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_huffman: tests/test_huffman.c $(OBJ_DIR)/huffman.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: $(TARGET) $(TEST_TARGETS)
	$(BUILD_DIR)/test_channels
	$(BUILD_DIR)/test_tuner
	$(BUILD_DIR)/test_db_concurrency
	$(BUILD_DIR)/test_stream_config
	$(BUILD_DIR)/test_stream_session
	$(BUILD_DIR)/test_transcode
	$(BUILD_DIR)/test_huffman
	tests/test_http_contract.sh

http-contract-test: $(TARGET)
	tests/test_http_contract.sh

install: $(TARGET)
	@echo "Installing ZapLink..."
	@if ! id -u zaplink > /dev/null 2>&1; then \
		useradd -r -s /usr/sbin/nologin zaplink; \
	fi
	install -d -o root -g root -m 755 $(INSTALL_DIR)
	install -d -o zaplink -g zaplink -m 750 $(STATE_DIR)
	@if getent group video >/dev/null; then usermod -aG video zaplink; fi
	@if getent group render >/dev/null; then usermod -aG render zaplink; fi
	install -o root -g root -m 755 $(TARGET) $(INSTALL_DIR)/zaplink
	install -o root -g root -m 644 huffman.bin $(INSTALL_DIR)/huffman.bin
	install -Dm644 support/zaplink.service $(SERVICE_DIR)/zaplink.service
	systemctl enable zaplink.service
	systemctl daemon-reload
	@echo "Installed. Service enabled. Start with: systemctl start zaplink"

.PHONY: all clean install test http-contract-test
