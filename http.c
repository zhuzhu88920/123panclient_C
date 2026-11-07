#include "pan.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

#define MAX_API_BODY (4u << 20)

struct HttpStream {
    HINTERNET session;
    HINTERNET connect;
    HINTERNET request;
};

typedef struct {
    char *host;
    char *path;
    INTERNET_PORT port;
    int secure;
} UrlParts;

static char g_err[512];

const char *http_error(void) { return g_err[0] ? g_err : "网络请求失败"; }

static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
}

/* 仅解析本项目会遇到的 scheme://host[:port]/path?query，全部 ASCII */
static int parse_url(const char *url, UrlParts *u)
{
    const char *p = strstr(url, "://");
    const char *hs, *he, *slash, *colon = NULL, *q;
    size_t hl;
    if (!p) return -1;
    memset(u, 0, sizeof *u);
    u->secure = (p - url == 5 && strncmp(url, "https", 5) == 0);
    hs = p + 3;
    slash = strchr(hs, '/');
    he = slash ? slash : hs + strlen(hs);
    for (q = hs; q < he; q++) if (*q == ':') { colon = q; break; }
    hl = (size_t)((colon ? colon : he) - hs);
    if (hl == 0 || hl >= 256) return -1;
    u->host = str_dup_n(hs, hl);
    if (!u->host) return -1;
    if (colon) {
        int po = atoi(colon + 1);
        u->port = (INTERNET_PORT)(po > 0 ? po : (u->secure ? 443 : 80));
    } else {
        u->port = (INTERNET_PORT)(u->secure ? 443 : 80);
    }
    u->path = str_dup(slash ? slash : "/");
    if (!u->path) { free(u->host); return -1; }
    return 0;
}

static void free_url(UrlParts *u)
{
    free(u->host); free(u->path);
    memset(u, 0, sizeof *u);
}

static wchar_t *widen(const char *s)
{
    wchar_t *w = NULL;
    if (utf8_to_utf16(s, &w)) return NULL;
    return w;
}

static int add_headers(HINTERNET req, const char **names, const char **vals, size_t nh)
{
    Buf b;
    wchar_t *w;
    BOOL ok;
    size_t i;
    if (nh == 0) return 0;
    if (buf_init(&b, 512)) return -1;
    for (i = 0; i < nh; i++) {
        if (buf_printf(&b, "%s: %s\r\n", names[i], vals[i])) { buf_free(&b); return -1; }
    }
    w = widen(b.data);
    buf_free(&b);
    if (!w) return -1;
    ok = WinHttpAddRequestHeaders(req, w, (DWORD)-1,
                                   WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    free(w);
    if (!ok) { set_err("设置请求头失败 (%lu)", (unsigned long)GetLastError()); return -1; }
    return 0;
}

/* 建立请求并接收响应头。成功时把 3 个句柄交给调用方，调用方负责关闭 */
static int open_request(const char *method, const char *url,
                        const char **names, const char **vals, size_t nh,
                        const void *body, size_t body_len, int follow_redirect,
                        struct HttpStream *s, int *status, char **location,
                        int64_t *content_length)
{
    UrlParts u;
    wchar_t *wagent = NULL, *whost = NULL, *wmethod = NULL, *wpath = NULL;
    DWORD flags, status_code = 0, sz;
    wchar_t loc[4096], cl[64];
    char *loc_u8 = NULL, *cl_u8 = NULL;
    int rc = -1;

    *status = 0;
    *location = NULL;
    *content_length = -1;
    memset(s, 0, sizeof *s);
    if (parse_url(url, &u)) { set_err("URL 无效"); return -1; }

    wagent = widen(APP_VERSION);
    whost = widen(u.host);
    wmethod = widen(method);
    wpath = widen(u.path);
    if (!wagent || !whost || !wmethod || !wpath) goto done;

    s->session = WinHttpOpen(wagent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s->session) { set_err("WinHttpOpen 失败 (%lu)", (unsigned long)GetLastError()); goto done; }
    WinHttpSetTimeouts(s->session, 10000, 10000, 10000, 30000);
    if (u.secure) {
        flags = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(s->session, WINHTTP_OPTION_SECURE_PROTOCOLS, &flags, sizeof flags);
    }
    s->connect = WinHttpConnect(s->session, whost, u.port, 0);
    if (!s->connect) { set_err("连接失败 (%lu)", (unsigned long)GetLastError()); goto done; }
    s->request = WinHttpOpenRequest(s->connect, wmethod, wpath, NULL,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!s->request) { set_err("创建请求失败 (%lu)", (unsigned long)GetLastError()); goto done; }
    if (!follow_redirect) {
        DWORD feature = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(s->request, WINHTTP_OPTION_DISABLE_FEATURE,
                         &feature, sizeof feature);
    }
    if (add_headers(s->request, names, vals, nh)) goto done;
    if (!WinHttpSendRequest(s->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             (LPVOID)body, (DWORD)body_len, (DWORD)body_len, 0)) {
        set_err("发送请求失败 (%lu)", (unsigned long)GetLastError());
        goto done;
    }
    if (!WinHttpReceiveResponse(s->request, NULL)) {
        set_err("接收响应失败 (%lu)", (unsigned long)GetLastError());
        goto done;
    }

    sz = sizeof status_code;
    if (WinHttpQueryHeaders(s->request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             NULL, &status_code, &sz,
                             NULL)) *status = (int)status_code;

    sz = sizeof loc;
    loc[0] = 0;
    if (WinHttpQueryHeaders(s->request, WINHTTP_QUERY_LOCATION,
                             NULL, loc, &sz, NULL) && loc[0]) {
        if (utf16_to_utf8(loc, &loc_u8) == 0) *location = loc_u8;
    }

    sz = sizeof cl;
    cl[0] = 0;
    if (WinHttpQueryHeaders(s->request, WINHTTP_QUERY_CONTENT_LENGTH,
                             NULL, cl, &sz, NULL) && cl[0]) {
        if (utf16_to_utf8(cl, &cl_u8) == 0) {
            long long v = strtoll(cl_u8, NULL, 10);
            if (v > 0) *content_length = (int64_t)v;
            free(cl_u8);
        }
    }
    rc = 0;

done:
    free(wagent); free(whost); free(wmethod); free(wpath);
    free_url(&u);
    if (rc != 0) {
        if (s->request) WinHttpCloseHandle(s->request);
        if (s->connect) WinHttpCloseHandle(s->connect);
        if (s->session) WinHttpCloseHandle(s->session);
        memset(s, 0, sizeof *s);
    }
    return rc;
}

static void close_handles(struct HttpStream *s)
{
    if (s->request) WinHttpCloseHandle(s->request);
    if (s->connect) WinHttpCloseHandle(s->connect);
    if (s->session) WinHttpCloseHandle(s->session);
    memset(s, 0, sizeof *s);
}

void http_response_init(HttpResponse *r)
{
    memset(r, 0, sizeof *r);
    buf_init(&r->body, 4096);
    r->content_length = -1;
}

void http_response_free(HttpResponse *r)
{
    free(r->location);
    buf_free(&r->body);
    memset(r, 0, sizeof *r);
}

int http_simple(const char *method, const char *url,
                const char **names, const char **vals, size_t nh,
                const char *body, size_t body_len, int follow_redirect,
                HttpResponse *r)
{
    struct HttpStream s;
    char *location = NULL, buf[65536];
    int status = 0, rc = -1;
    int64_t clen = -1;
    DWORD got = 0;

    http_response_init(r);
    if (open_request(method, url, names, vals, nh, body, body_len,
                     follow_redirect, &s, &status, &location, &clen)) {
        http_response_free(r);
        return -1;
    }
    r->status = status;
    r->location = location;
    r->content_length = clen;
    while (WinHttpReadData(s.request, buf, sizeof buf, &got) && got > 0) {
        if (r->body.len + got > MAX_API_BODY) {
            set_err("API 响应超过 4MB 上限");
            goto done;
        }
        if (buf_append(&r->body, buf, got)) goto done;
    }
    rc = 0;
done:
    close_handles(&s);
    if (rc != 0) http_response_free(r);
    return rc;
}

int http_stream_open(const char *url, const char *range, HttpStream **out,
                     int *status, int64_t *content_length)
{
    struct HttpStream tmp;
    const char *names[2], *vals[2];
    size_t nh = 1;
    char *location = NULL;
    int st = 0;
    int64_t cl = -1;
    HttpStream *s;

    *out = NULL;
    names[0] = "User-Agent"; vals[0] = APP_VERSION;
    if (range && *range) { names[1] = "Range"; vals[1] = range; nh = 2; }
    if (open_request("GET", url, names, vals, nh, NULL, 0, 0,
                     &tmp, &st, &location, &cl)) return -1;
    free(location);
    s = (HttpStream *)xmalloc(sizeof *s);
    if (!s) { close_handles(&tmp); return -1; }
    *s = tmp;
    *out = s;
    *status = st;
    *content_length = cl;
    return 0;
}

int http_stream_read(HttpStream *s, void *buf, size_t cap, size_t *got)
{
    DWORD n = 0;
    *got = 0;
    if (!s || cap == 0 || cap > 0xFFFFFFFFu) return -1;
    if (!WinHttpReadData(s->request, buf, (DWORD)cap, &n)) {
        set_err("读取下载数据失败 (%lu)", (unsigned long)GetLastError());
        return -1;
    }
    *got = n;
    return 0;
}

void http_stream_close(HttpStream *s)
{
    if (!s) return;
    close_handles(s);
    free(s);
}
