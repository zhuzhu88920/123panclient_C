#include "pan.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

static HANDLE g_hout = NULL;
static int g_console = 0;

void out_init(void)
{
    DWORD mode = 0;
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_hout && g_hout != INVALID_HANDLE_VALUE && GetConsoleMode(g_hout, &mode))
        g_console = 1;
}

int stdout_is_console(void) { return g_console; }

/* 真控制台走 WriteConsoleW（不受代码页影响）；重定向/管道写 UTF-8 字节 */
void out(const char *fmt, ...)
{
    Buf b;
    va_list ap, ap2;
    int n;
    if (buf_init(&b, 256)) return;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n >= 0 && !buf_reserve(&b, (size_t)n + 1)) {
        vsnprintf(b.data, (size_t)n + 1, fmt, ap2);
        b.len = (size_t)n;
    }
    va_end(ap2);
    if (g_console && b.len) {
        wchar_t *w = NULL;
        if (utf8_to_utf16(b.data, &w) == 0) {
            DWORD wr = 0;
            if (WriteConsoleW(g_hout, w, (DWORD)wcslen(w), &wr, NULL)) {
                free(w);
                buf_free(&b);
                return;
            }
            free(w);
        }
    }
    if (b.len) { fwrite(b.data, 1, b.len, stdout); fflush(stdout); }
    buf_free(&b);
}

int read_line(char **line)
{
    HANDLE h;
    DWORD mode = 0;
    if (!line) return 0;
    *line = NULL;
    h = GetStdHandle(STD_INPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        wchar_t w[4096];
        DWORD n = 0;
        if (!ReadConsoleW(h, w, 4095, &n, NULL)) return 0;
        w[n] = 0;
        while (n > 0 && (w[n - 1] == L'\n' || w[n - 1] == L'\r')) w[--n] = 0;
        return utf16_to_utf8(w, line) == 0;
    }
    {
        char b[4096];
        size_t n;
        if (!fgets(b, sizeof b, stdin)) return 0;
        n = strlen(b);
        while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
        *line = str_dup_n(b, n);
        return *line != NULL;
    }
}
