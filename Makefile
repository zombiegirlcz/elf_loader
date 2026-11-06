CC = gcc
CFLAGS = -Wall -Wextra -g -O0 -std=c11 -B/usr/bin
LDFLAGS = -ldl

TARGET = elf_loader
SRCS = src/main.c src/elf_loader.c src/ldso_tls.c src/timing.c src/cache.c src/imgcache.c src/entry.S src/l1_common.S
OBJS = src/main.o src/elf_loader.o src/ldso_tls.o src/timing.o src/cache.o src/imgcache.o src/entry.o src/l1_common.o

HOSTCC ?= gcc

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

test/mods/libmod.so: test/mod.c | test/mods
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $<

test/mods:
	mkdir -p $@

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

# Host-only unit testy (zadne zarizeni, zadny rootfs): kompiluji se hostim
# gcc/clang a testuji cistou logiku hlavicek (binds.h, proot_compat.h).
# Bezi v CI i lokalne: `make test-host`.
test-host:
	@echo "== binds: bind_parse_spec (identity bind bez dvojtecky) =="
	$(HOSTCC) -I./src -o /tmp/bps_test test/binds_parse_spec_test.c
	/tmp/bps_test
	@echo "== binds: bind_reverse pred/po g_binds_ready =="
	$(HOSTCC) -I./src -o /tmp/bg_test test/binds_gready_test.c
	/tmp/bg_test
	@echo "== proot_compat: parser =="
	$(HOSTCC) -I./src -o /tmp/pc_test test/proot_compat_test.c
	/tmp/pc_test
	@echo "== verdef: elf_file_has_verdef_prefix (Bun detekce ze souboru) =="
	$(HOSTCC) -I./src -o /tmp/vd_test test/verdef_file_test.c
	/tmp/vd_test

clean:
	rm -f $(OBJS) $(TARGET) test/hello test/ifunc test/use_mod
	rm -f test/env test/math test/tls test/tls-gd test/uselib test/libtls.so
	rm -rf test/mods

.PHONY: all clean test
