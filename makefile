CC = gcc
# Common CFLAGS for compiling. -pthread is needed.
CFLAGS = -Wall -Wextra -pthread -g -std=c99
# Linker flags are often managed by CFLAGS when compiling and linking in one step,
# or can be separate. -lpthread is crucial for linking.
LDFLAGS = -lpthread

# The executable name
TARGET = LogAnalyzer

# Source files for the LogAnalyzer executable
# Ensure your main C file is named 200104004045_main.c
SOURCES = 200104004045_main.c buffer.c

# Default rule: build the LogAnalyzer executable
all: $(TARGET)

# Rule to create the LogAnalyzer executable
# This compiles all source files and links them together in one step,
# directly producing the TARGET executable without explicit .o files in the Makefile.
$(TARGET): $(SOURCES) buffer.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES) $(LDFLAGS)

# Clean rule: removes the executable
clean:
	rm -f $(TARGET)

.PHONY: all clean