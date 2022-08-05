#include <stdarg.h>
#include <stdio.h>

#include "common.h"

void say__(int flag, char *fmt, ...){
    if (!flag) return;
    va_list va;
    va_start(va, fmt);
    vfprintf(stdout, fmt, va);
    va_end(va);
}

