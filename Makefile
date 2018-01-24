# Makefile for scEmacs
#
# NoDebug --> CFLAGS =  -Wall -I/usr/include/freetype2
# Debug   --> CFLAGS =  -g3 -Wall -DDEBUG -I/usr/include/freetype2

all: scEmacs

CC ?= gcc
CFLAGS =  -g3 -Wall -DDEBUG -I/usr/include/freetype2
Objects = sc_Main.o sc_Editor.o

scEmacs: $(Objects)
	$(CC) $(Objects) -o scEmacs -lX11 -lXft

sc_Main.o: sc_Main.c sc_Main.h sc_Editor.h sc_Error.h sc_Public.h
	$(CC) $(CFLAGS)  -c sc_Main.c

sc_Editor.o: sc_Editor.c sc_Main.h sc_Editor.h sc_Error.h sc_Public.h
	$(CC) $(CFLAGS) -c sc_Editor.c

.PHONY : clean
clean:
	-rm scEmacs $(Objects)


