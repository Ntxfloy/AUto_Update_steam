# Makefile for Steam Auto-Updater client (MinGW-w64)
#   cd client && mingw32-make
# Output: dist/steam_updater.exe

CC       = gcc
SRC_DIR  = src
TARGET   = dist/steam_updater.exe

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/config.c \
       $(SRC_DIR)/acf.c \
       $(SRC_DIR)/catalog.c \
       $(SRC_DIR)/log.c \
       $(SRC_DIR)/steamcmd.c \
       $(SRC_DIR)/api.c \
       $(SRC_DIR)/lease.c \
       $(SRC_DIR)/worker.c

CFLAGS  = -Wall -Wextra -O2 -DWINVER=0x0601 -D_WIN32_WINNT=0x0601
LDFLAGS = -mwindows -lwinhttp -lole32 -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -luxtheme -ldwmapi

all: $(TARGET)

$(TARGET): $(SRCS)
	@if not exist dist mkdir dist
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

debug: CFLAGS += -g -O0 -DDEBUG
debug: LDFLAGS := $(filter-out -mwindows,$(LDFLAGS))
debug: $(TARGET)

clean:
	-del /q dist\steam_updater.exe

.PHONY: all debug clean
