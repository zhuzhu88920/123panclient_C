#include "pan.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

const char *APP_VERSION = "123pan/v2.4.0(Android_7.1.2;Xiaomi)";
volatile long g_interrupted = 0;

void *xmalloc(size_t n) { return malloc(n ? n : 1); }

char *str_dup_n(const char *s, size_t n)
{
    char *p = (char *)xmalloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *str_dup(const char *s) { return s ? str_dup_n(s, strlen(s)) : NULL; }

int buf_init(Buf *b, size_t cap)
{
    if (cap < 64) cap = 64;
    b->data = (char *)xmalloc(cap);
    if (!b->data) return -1;
    b->cap = cap; b->len = 0; b->data[0] = 0;
    return 0;
}

void buf_free(Buf *b)
{
    if (b->data) free(b->data);
    b->data = NULL; b->len = 0; b->cap = 0;
}

/* need 为「len + 追加量 + 1」，始终为结尾 NUL 预留一个字节 */
int buf_reserve(Buf *b, size_t need)
{
    size_t cap;
    char *p;
    if (!b->data) return buf_init(b, need);
    if (need <= b->cap) return 0;
    cap = b->cap;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) return -1;
        cap *= 2;
    }
    p = (char *)realloc(b->data, cap);
    if (!p) return -1;
    b->data = p; b->cap = cap;
    return 0;
}

int buf_append(Buf *b, const void *p, size_t n)
{
    if (n > SIZE_MAX - b->len - 1) return -1;
    if (buf_reserve(b, b->len + n + 1)) return -1;
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}

int buf_append_cstr(Buf *b, const char *s) { return buf_append(b, s, strlen(s)); }

int buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    int n;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return -1; }
    if (buf_reserve(b, b->len + (size_t)n + 1)) { va_end(ap2); return -1; }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    b->data[b->len] = 0;
    return 0;
}

char *buf_take(Buf *b)
{
    char *s = str_dup_n(b->data ? b->data : "", b->len);
    buf_free(b);
    return s;
}

int utf8_to_utf16(const char *s, wchar_t **out)
{
    int n;
    wchar_t *w;
    if (!s || !out) return -1;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return -1;
    w = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t));
    if (!w) return -1;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) {
        free(w); return -1;
    }
    *out = w;
    return 0;
}

int utf16_to_utf8(const wchar_t *s, char **out)
{
    int n;
    char *p;
    if (!s || !out) return -1;
    n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return -1;
    p = (char *)xmalloc((size_t)n);
    if (!p) return -1;
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, p, n, NULL, NULL) <= 0) {
        free(p); return -1;
    }
    *out = p;
    return 0;
}

int utf8_next(const char *s, size_t len, size_t *i, uint32_t *cp)
{
    size_t k = *i, j;
    unsigned char c;
    uint32_t v;
    size_t need;
    if (k >= len) return 0;
    c = (unsigned char)s[k];
    if (c < 0x80)      { v = c;        need = 1; }
    else if ((c & 0xE0) == 0xC0) { v = c & 0x1F; need = 2; }
    else if ((c & 0xF0) == 0xE0) { v = c & 0x0F; need = 3; }
    else if ((c & 0xF8) == 0xF0) { v = c & 0x07; need = 4; }
    else { *i = k + 1; *cp = 0xFFFD; return 1; }
    if (need > len - k) { *i = len; *cp = 0xFFFD; return 1; }
    for (j = 1; j < need; j++) {
        unsigned char cc = (unsigned char)s[k + j];
        if ((cc & 0xC0) != 0x80) { *i = k + 1; *cp = 0xFFFD; return 1; }
        v = (v << 6) | (cc & 0x3F);
    }
    *i = k + need;
    *cp = v;
    return 1;
}

int utf8_append_cp(Buf *b, uint32_t cp)
{
    char tmp[4];
    size_t n = 0;
    if (cp < 0x80) {
        tmp[n++] = (char)cp;
    } else if (cp < 0x800) {
        tmp[n++] = (char)(0xC0 | (cp >> 6));
        tmp[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        tmp[n++] = (char)(0xE0 | (cp >> 12));
        tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        tmp[n++] = (char)(0xF0 | (cp >> 18));
        tmp[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[n++] = (char)(0x80 | (cp & 0x3F));
    }
    return buf_append(b, tmp, n);
}

int utf8_width(const char *s)
{
    size_t i = 0, len = strlen(s);
    uint32_t cp;
    int w = 0;
    while (utf8_next(s, len, &i, &cp)) w += (cp >= 0x2E80) ? 2 : 1;
    return w;
}

/* 只转义内容，不补两端引号；非 ASCII 字节按 UTF-8 原样输出 */
int json_escape(Buf *b, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!s) return -1;
    for (; *p; p++) {
        switch (*p) {
        case '"':  if (buf_append(b, "\\\"", 2)) return -1; break;
        case '\\': if (buf_append(b, "\\\\", 2)) return -1; break;
        case '\n': if (buf_append(b, "\\n", 2)) return -1; break;
        case '\r': if (buf_append(b, "\\r", 2)) return -1; break;
        case '\t': if (buf_append(b, "\\t", 2)) return -1; break;
        default:
            if (*p < 0x20) { if (buf_printf(b, "\\u%04x", (unsigned)*p)) return -1; }
            else if (buf_append(b, p, 1)) return -1;
        }
    }
    return 0;
}

int is_space_ch(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int64_t now_unix(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

void now_cst12(char out[13])
{
    FILETIME ft;
    ULARGE_INTEGER u;
    SYSTEMTIME st;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += (uint64_t)8 * 3600 * 10000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    snprintf(out, 13, "%04d%02d%02d%02d%02d",
             (int)st.wYear, (int)st.wMonth, (int)st.wDay,
             (int)st.wHour, (int)st.wMinute);
}

uint64_t tick_ms(void) { return (uint64_t)GetTickCount64(); }
