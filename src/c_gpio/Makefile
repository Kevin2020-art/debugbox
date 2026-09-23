CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -pthread -std=c99
LDFLAGS ?= -pthread -lgpiod

TARGET_DAEMON = debugbox_gpio_daemon
TARGET_CTL = gpio_ctl

SRCS = gpio_controller.c main.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET_DAEMON) $(TARGET_CTL)

$(TARGET_DAEMON): gpio_controller.o main.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_CTL): $(TARGET_DAEMON)
	ln -sf $(TARGET_DAEMON) $(TARGET_CTL)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(TARGET_DAEMON) $(TARGET_CTL)

install: all
	install -d /usr/local/bin
	install -m 755 $(TARGET_DAEMON) /usr/local/bin/
	install -m 755 $(TARGET_CTL) /usr/local/bin/

.PHONY: all clean install
