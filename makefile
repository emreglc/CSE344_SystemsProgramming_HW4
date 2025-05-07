CC = gcc
# Use -std=c99 or -std=c11 if specific C standard features are used and compiler defaults are older.
# _GNU_SOURCE for getline is common, -std=gnu99 or similar implies it.
# For portability and common use:
CFLAGS = -Wall -Wextra -pthread -g -std=c99
LDFLAGS = -lpthread

# The executable name
TARGET = LogAnalyzer

# Source files - StudentID_main.c should be your actual filename
# For this example, assuming the main file is named StudentID_main.c
SOURCES = StudentID_main.c buffer.c
OBJECTS = $(SOURCES:.c=.o)

# Default rule
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LDFLAGS)

# Rule to compile .c files to .o files
# buffer.h is a dependency for both .c files that include it.
StudentID_main.o: StudentID_main.c buffer.h
	$(CC) $(CFLAGS) -c StudentID_main.c -o StudentID_main.o

buffer.o: buffer.c buffer.h
	$(CC) $(CFLAGS) -c buffer.c -o buffer.o

# Clean rule
clean:
	rm -f $(TARGET) $(OBJECTS)

.PHONY: all clean