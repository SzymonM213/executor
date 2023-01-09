#include "safe_printf.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

void safe_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&mtx);
    vfprintf(stdout, fmt, ap);
    fflush(stdout);
    pthread_mutex_unlock(&mtx);
    va_end(ap);
}