#include "pan.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static FILE *g_log = NULL;

int log_init(const char *dir)
{
    Buf p;
    wchar_t *w = NULL;
    SYSTEMTIME st;
    if (buf_init(&p, 256)) return -1;
    if (buf_printf(&p, "%s\\logs", dir)) { buf_free(&p); return -1; }
    if (utf8_to_utf16(p.data, &w) == 0) { CreateDirectoryW(w, NULL); free(w); }
    buf_free(&p);

    GetLocalTime(&st);
    if (buf_init(&p, 256)) return -1;
    if (buf_printf(&p, "%s\\logs\\%04d-%02d-%02d.log",
                   dir, (int)st.wYear, (int)st.wMonth, (int)st.wDay)) {
        buf_free(&p); return -1;
    }
    w = NULL;
    if (utf8_to_utf16(p.data, &w) == 0) { g_log = _wfopen(w, L"ab"); free(w); }
    buf_free(&p);
    return g_log ? 0 : -1;
}

void log_close(void)
{
    if (g_log) { fclose(g_log); g_log = NULL; }
}

/* 调用方必须保证 msg 不含凭据、token 与签名 URL */
void log_only(const char *level, const char *msg)
{
    SYSTEMTIME st;
    if (!g_log) return;
    GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d [%s] %s\n",
            (int)st.wHour, (int)st.wMinute, (int)st.wSecond, level, msg);
    fflush(g_log);
}
