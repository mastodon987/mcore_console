CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
TARGET  = mcore

# Try to build with readline; fall back to basic if not installed
READLINE_LIBS = -lreadline

.PHONY: all clean install

all:
	@if pkg-config --exists readline 2>/dev/null; then \
		echo "Building with readline support"; \
		$(CC) $(CFLAGS) -o $(TARGET) mcore.c $(READLINE_LIBS); \
	else \
		echo "readline not found -- building without line editing (-DNO_READLINE)"; \
		$(CC) $(CFLAGS) -DNO_READLINE -o $(TARGET) mcore.c; \
	fi

readline:
	$(CC) $(CFLAGS) -o $(TARGET) mcore.c $(READLINE_LIBS)

no-readline:
	$(CC) $(CFLAGS) -DNO_READLINE -o $(TARGET) mcore.c

install: all
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET)
