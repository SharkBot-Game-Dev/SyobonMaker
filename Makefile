CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -Iinclude

LDFLAGS  = -mwindows -lcomdlg32

TARGET   = patcher.exe
SRCS     = main.c
OBJS     = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
