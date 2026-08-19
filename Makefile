CC = gcc
CFLAGS = -Wall -Wextra -g -O0 -std=c11 -B/usr/bin
LDFLAGS = -ldl

TARGET = elf_loader
SRCS = src/main.c src/elf_loader.c src/entry.S
OBJS = src/main.o src/elf_loader.o src/entry.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c include/elf_loader.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

test/hello: test/hello.c
	$(CC) $(CFLAGS) -o $@ $<

test/ifunc: test/ifunc.c
	$(CC) $(CFLAGS) -o $@ $<

test/mods/libmod.so: test/mod.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $<

test/use_mod: test/use_mod.c test/mods/libmod.so
	$(CC) $(CFLAGS) -o $@ $< -Ltest/mods -lmod

test: $(TARGET) test/hello test/ifunc test/mods/libmod.so test/use_mod
	@echo "== introspect /bin/ls =="
	./$(TARGET) /bin/ls 2>&1 | tail -8
	@echo "== execute test/hello =="
	./$(TARGET) --run test/hello foo bar 2>&1 | tail -6
	@echo "== execute test/ifunc (IRELATIVE) =="
	./$(TARGET) --run test/ifunc 2>&1 | tail -1
	IFUNC_MODE=s ./$(TARGET) --run test/ifunc 2>&1 | tail -1
	@echo "== execute test/uselib (TLS via .so) =="
	./$(TARGET) --run test/uselib 2>&1 | tail -2
	@echo "== own module loader (private scope) =="
	./$(TARGET) --own test/use_mod test/mods/libmod.so 2>&1 | tail -4
	@echo "== own module loader + module TLS (TLSDESC) =="
	./$(TARGET) --own test/uselib test/libtls.so 2>&1 | tail -2
	@echo "== execute test/hello with interposed puts =="
	./$(TARGET) --shim test/hello 2>&1 | tail -3
	@echo "== execute test/hello with lazy PLT binding =="
	./$(TARGET) --lazy test/hello foo bar 2>&1 | tail -4

clean:
	rm -f $(OBJS) $(TARGET) test/hello test/ifunc test/mods/libmod.so test/use_mod

.PHONY: all clean test
