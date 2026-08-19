#include <stdio.h>
static __thread int lib_var = 7;
int lib_get(void) { return lib_var; }
int lib_bump(void) { return ++lib_var; }
