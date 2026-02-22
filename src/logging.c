#include "logging.h"

#include <stdarg.h>
#include <stdio.h>

void log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("[INFO]\t", stdout);
    vfprintf(stdout, fmt, args);
    fputc('\n', stdout);
    va_end(args);
}

void log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("[ERROR]\t", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}
