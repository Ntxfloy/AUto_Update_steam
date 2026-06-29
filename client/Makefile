# Makefile for Steam Auto-Updater client
# Compiler: MinGW-w64 (gcc) via Strawberry Perl or standalone MinGW
# Usage:
#   mingw32-make          - build release  -> dist/steam_updater.exe
#   mingw32-make debug    - build with debug info
#   mingw32-make clean    - remove build artifacts

CC      = gcc
DIST_DIR = dist
OBJ_DIR  = obj
TARGET   = $(DIST_DIR)/steam_updater.exe
SRC_DIR  = src

SRCS = $(SRC_DIR)/main.c     \
       $(SRC_DIR)/acf.c      \
       $(SRC_DIR)/steamcmd.c \
       $(SRC_DIR)/api.c      \
       $(SRC_DIR)/worker.c   \
       $(SRC_DIR)/config.c   \
       $(SRC_DIR)/lease.c

OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

CFLAGS  = -Wall -Wextra -O2 -DWINVER=0x0601 -D_WIN32_WINNT=0x0601
LDFLAGS = -mwindows -lwinhttp -lole32 -lcomctl32 -lcomdlg32

# Debug build
DEBUG_CFLAGS = -Wall -Wextra -g -DDEBUG -DWINVER=0x0601 -D_WIN32_WINNT=0x0601

.PHONY: all debug clean

all: $(OBJ_DIR) $(DIST_DIR) $(TARGET)

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: $(OBJ_DIR) $(DIST_DIR) $(TARGET)

$(OBJ_DIR):
	if not exist $(OBJ_DIR) mkdir $(OBJ_DIR)

$(DIST_DIR):
	if not exist $(DIST_DIR) mkdir $(DIST_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "=== Built: $(TARGET) ==="

clean:
	if exist $(OBJ_DIR) rmdir /s /q $(OBJ_DIR)
	if exist $(DIST_DIR) rmdir /s /q $(DIST_DIR)
