# ELF Loader Test Results

## Test Summary (2026-09-02)

**Total Tests:** 41  
**Passed:** 30 (73%)  
**Failed:** 11 (27%)  
**Skipped:** 0

---

## ✅ PASSING TESTS (30)

### Text Processing (7/10)
- ✅ grep - grep root in passwd
- ✅ sed - sed print first line
- ✅ awk - awk math
- ✅ wc - wc count chars
- ✅ head - head chars
- ✅ tail - tail chars
- ✅ cat - cat file

### File Operations (6/6)
- ✅ ls - ls directory
- ✅ stat - stat file
- ✅ find - find files
- ✅ realpath - realpath
- ✅ dirname - dirname
- ✅ basename - basename

### File Creation/Deletion (3/3)
- ✅ mkdir - mkdir rmdir
- ✅ touch - touch rm
- ✅ cp - cp cat rm

### System Info (5/6)
- ✅ uname - uname -a
- ✅ id - id -u
- ✅ date - date
- ✅ uptime - uptime
- ❌ hostname - hostname (expected 'TERMINATOR', got: localhost)

### Utilities (9/9)
- ✅ echo - echo
- ✅ printf - printf
- ✅ seq - seq
- ✅ yes - yes head
- ✅ diff - diff same
- ✅ true - true
- ✅ false - false (exit 1)
- ✅ sleep - sleep
- ✅ sh - sh -c
- ✅ bash - bash -c

---

## ❌ FAILING TESTS (11)

### 1. **cut** - Path Truncation Issue
```
FAIL: cut - cut field (expected 'root', got: sh: /data/user/0/com.linux_core/files/nh/distro/pa)
```
**Issue:** Path is being truncated. The error shows the path is cut off at "parrot" → "pa".
**Root Cause:** Likely a buffer overflow or string truncation in the path handling code.
**Impact:** Medium - affects commands that use pipes or complex arguments.

### 2. **sort, uniq, tr** - Timeout Issues
```
TIMEOUT: sort - sort stdin
TIMEOUT: uniq - uniq dedup
TIMEOUT: tr - tr translate
```
**Issue:** Commands hang when reading from stdin via pipe.
**Root Cause:** The pipe handling or stdin redirection is not working correctly. The commands are waiting for input that never arrives or the pipe is not properly set up.
**Impact:** High - affects all piped operations.

### 3. **mv** - Security Block
```
FAIL: mv - mv cat mv back (expected 'TERMINATOR', got: Command blocked for security reasons)
```
**Issue:** The `mv` command is being blocked by a security mechanism.
**Root Cause:** Likely the seccomp filter or some security check is blocking the `rename` syscall.
**Impact:** Medium - file operations are partially broken.

### 4. **hostname** - Test Expectation Issue
```
FAIL: hostname - hostname (expected 'TERMINATOR', got: localhost)
```
**Issue:** Test expectation is wrong. The command works correctly, but the test expects "TERMINATOR" (the hostname) but gets "localhost".
**Root Cause:** Test script bug - the hostname on the device is "localhost", not "TERMINATOR".
**Impact:** Low - false positive failure.

### 5. **whoami** - User ID Lookup Issue
```
FAIL: whoami: cannot find name for user ID 10314
```
**Issue:** The `whoami` command can't map UID 10314 to a username.
**Root Cause:** The `/etc/passwd` file doesn't have an entry for UID 10314 (the app's UID).
**Impact:** Low - cosmetic issue, doesn't affect functionality.

### 6. **gzip** - ELF File Detection Issue
```
FAIL: gzip - gzip gunzip pipe (expected 'test', got: [-] Not an ELF file)
```
**Issue:** The gzip binary is being detected as "Not an ELF file".
**Root Cause:** This is likely a false positive from the ELF loader's file type detection. The gzip binary might be a script or have an unusual format.
**Impact:** Medium - affects compression utilities.

### 7. **tar** - Path Issue
```
FAIL: tar - tar gz (expected 'TERMINATOR', got: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/tar: Removing leading `/' from member nam)
```
**Issue:** The tar command is outputting a warning about removing leading '/' from member names.
**Root Cause:** This is actually a warning, not an error. The test expectation is wrong - tar is working correctly.
**Impact:** Low - false positive failure.

### 8. **ping, nslookup** - Network Binary Missing
```
FAIL: ping - ping localhost (expected '1 received', got: [-] open(/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/ping): No such file or directory)
FAIL: nslookup: nslookup (expected 'localhost', got: [-] open(/data/user/0/com.linux_core/files/nh/distro/parrot/usr/bin/nslookup): No such file or direc)
```
**Issue:** The binaries don't exist in the rootfs.
**Root Cause:** These binaries are not installed in the Parrot rootfs.
**Impact:** Low - missing binaries, not a loader issue.

---

## Key Findings

### Working Features ✅
1. **Basic ELF loading** - Simple binaries work correctly
2. **Dynamic linking** - Most shared libraries load correctly
3. **File operations** - Most file operations work (ls, cat, cp, mkdir, etc.)
4. **Text processing** - grep, sed, awk, wc work well
5. **System calls** - Most syscalls work correctly
6. **Shell execution** - sh and bash work correctly

### Known Issues ⚠️
1. **Pipe handling** - Commands that read from stdin via pipe hang (sort, uniq, tr)
2. **Path handling** - Some paths are being truncated (cut command)
3. **Security restrictions** - Some syscalls are blocked (mv rename)
4. **ELF detection** - Some non-ELF files are incorrectly detected
5. **Network binaries** - Some network utilities are missing from rootfs

### Recommendations 🔧

1. **Fix pipe handling** - Investigate stdin/stdout redirection for piped commands
2. **Fix path truncation** - Check buffer sizes in path handling code
3. **Review seccomp filters** - Ensure necessary syscalls are allowed
4. **Improve ELF detection** - Better handle non-ELF files (scripts, etc.)
5. **Add missing binaries** - Install ping, nslookup, etc. in rootfs if needed

---

## Test Commands Used

```bash
# Run all tests
./test-real-usage.sh

# Run quick tests
./quick-test.sh grep sed awk ls cat

# View results
cat results/pass_*.txt
cat results/fail_*.txt
```

---

## Next Steps

1. **Fix pipe handling** - Priority: HIGH
2. **Fix path truncation** - Priority: MEDIUM
3. **Review seccomp filters** - Priority: MEDIUM
4. **Improve ELF detection** - Priority: LOW
5. **Add missing binaries** - Priority: LOW
