CC = gcc
CFLAGS = -Wall -Wextra -I./include -I./deps/include -g
LDFLAGS = -L./deps/lib -lsqlite3 -lpthread

SRC_DIR = src
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

INSTALL_DIR = /opt/zaplink
SERVICE_DIR = /etc/systemd/system

SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/channels.c $(SRC_DIR)/db.c $(SRC_DIR)/epg.c \
       $(SRC_DIR)/huffman.c $(SRC_DIR)/scanner.c $(SRC_DIR)/tuner.c \
       $(SRC_DIR)/transcode.c $(SRC_DIR)/http_server.c $(SRC_DIR)/benchmark.c \
       $(SRC_DIR)/thread_pool.c

OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

TARGET = $(BUILD_DIR)/zaplink

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Build complete: $@"


$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

install: $(TARGET)
	@echo "Installing ZapLink..."
	@mkdir -p $(INSTALL_DIR)
	@if ! id -u zaplink > /dev/null 2>&1; then \
		useradd -r -s /usr/sbin/nologin zaplink; \
	fi
	cp -f $(TARGET) $(INSTALL_DIR)/zaplink
	cp -n huffman.bin $(INSTALL_DIR)/ || true
	cp -f support/zaplink.service $(INSTALL_DIR)/
	chown -R zaplink:zaplink $(INSTALL_DIR)
	systemctl link $(INSTALL_DIR)/zaplink.service
	systemctl enable zaplink
	systemctl daemon-reload
	@echo "Installed. Service enabled. Start with: systemctl start zaplink"

.PHONY: all clean install
