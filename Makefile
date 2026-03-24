# ============================================================================
# Makefile — TTMS 简化编译（部分模块，完整编译请用 run.bat）
# 依赖：MinGW gcc 或 Linux gcc
# ============================================================================

CC = gcc
CFLAGS = -finput-charset=UTF-8 -fexec-charset=UTF-8 -I src -I src/View -I src/Service -I src/Persistence -I src/Common
TARGET = TTMS.exe

SRCS = TTMS.c \
       src/View/Main_Menu.c \
       src/View/Studio_UI.c \
       src/View/Seat_UI.c \
       src/Service/Studio.c \
       src/Service/Seat.c \
       src/Persistence/Studio_Persist.c \
       src/Persistence/EntityKey_Persist.c \
       src/Persistence/Data_Store.c \
       src/Persistence/Seat_Persist.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	REM 仅清理 .o 文件，保留 TTMS.exe
	del /Q src\*.o src\View\*.o src\Service\*.o src\Persistence\*.o 2>nul

run: $(TARGET)
	.\$(TARGET)

.PHONY: all clean run
