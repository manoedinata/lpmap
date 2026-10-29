CC = clang
CFLAGS = --target=aarch64-linux-gnu -O2 -Wall -ffreestanding -fno-stack-protector \
	-fno-builtin -nostdlib -static -fuse-ld=lld

lpmap: lpmap.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f lpmap

.PHONY: clean
