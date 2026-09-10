# Steam Auto-Updater client - MinGW-w64 makefile
# Build:  mingw32-make
# Debug:  mingw32-make debug
# Clean:  mingw32-make clean

CC       = gcc
SRC_DIR  = src
OBJ_DIR  = obj
DIST_DIR = dist
TARGET   = $(DIST_DIR)/steam_updater.exe

CFLAGS   = -Wall -Wextra -O2 -DWINVER=0x0601 -D_WIN32_WINNT=0x0601
LDFLAGS  = -mwindows -lwinhttp -lole32 -lcomctl32 -lcomdlg32

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/config.c \
       $(SRC_DIR)/acf.c \
       $(SRC_DIR)/catalog.c \
       $(SRC_DIR)/steamcmd.c \
       $(SRC_DIR)/api.c \
       $(SRC_DIR)/lease.c \
       $(SRC_DIR)/worker.c

OBJS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all debug clean

all: $(TARGET)

$(TARGET): $(OBJS) | $(DIST_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	-mkdir $(OBJ_DIR)

$(DIST_DIR):
	-mkdir $(DIST_DIR)

debug: CFLAGS += -g -O0 -DDEBUG
debug: LDFLAGS := $(filter-out -mwindows,$(LDFLAGS))
debug: clean all

clean:
	-del /Q $(OBJ_DIR)\*.o 2>nul
	-del /Q $(DIST_DIR)\steam_updater.exe 2>nul
