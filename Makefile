# Makefile nativo per AmigaOS 4.1

CC = gcc
STRIP = strip

# Su OS4 l'SDK è solitamente assegnato a SDK:
INCLUDES = -I./src -ISDK:Include/include_h
CFLAGS = -O2 -Wall $(INCLUDES)

SRCS = src/main.c src/virtio_pci.c src/virtqueue.c
OBJS = $(SRCS:.c=.o)
TARGET = virtio-serial.device

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -nostartfiles -o $@ $(OBJS)
	$(STRIP) --strip-unneeded $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	delete $(OBJS) $(TARGET)
