# Makefile for the custom shell project

CC = gcc
CFLAGS = -Wall -g
TARGET = myshell

all: $(TARGET)

$(TARGET): myshell.c
	$(CC) $(CFLAGS) -o $(TARGET) myshell.c

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean
