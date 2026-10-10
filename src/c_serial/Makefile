CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -Iinclude -D_GNU_SOURCE -D_DEFAULT_SOURCE
LDFLAGS ?= 

SRCDIR = src
INCDIR = include
OBJDIR = obj
BINDIR = bin

DAEMON_SRCS = $(SRCDIR)/main.c $(SRCDIR)/serial_port.c $(SRCDIR)/log_rotator.c $(SRCDIR)/client_hub.c
DAEMON_OBJS = $(OBJDIR)/main.o $(OBJDIR)/serial_port.o $(OBJDIR)/log_rotator.o $(OBJDIR)/client_hub.o
DAEMON_BIN  = $(BINDIR)/debugbox_serial_daemon

CONSOLE_SRCS = $(SRCDIR)/console_cli.c
CONSOLE_OBJS = $(OBJDIR)/console_cli.o
CONSOLE_BIN  = $(BINDIR)/debug-console

all: $(DAEMON_BIN) $(CONSOLE_BIN)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(DAEMON_BIN): $(DAEMON_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) $(DAEMON_OBJS) $(LDFLAGS) -o $@

$(CONSOLE_BIN): $(CONSOLE_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) $(CONSOLE_OBJS) $(LDFLAGS) -o $@

install: all
	install -d /usr/local/bin
	install -m 755 $(DAEMON_BIN) /usr/local/bin/
	install -m 755 $(CONSOLE_BIN) /usr/local/bin/

clean:
	rm -rf $(OBJDIR) $(BINDIR)

.PHONY: all install clean
