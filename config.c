#include "pan.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

char *exe_dir(void)
{
    wchar_t w[4096];
    DWORD n = GetModuleFileNameW(NULL, w, 4096);
    char *u = NULL, *slash;
    if (!n || n >= 4096) return str_dup(".");
    w[n] = 0;
    if (utf16_to_utf8(w, &u)) return str_dup(".");
    slash = strrchr(u, '\\');
    if (!slash) { free(u); return str_dup("."); }
    *slash = 0;
    return u;
}

static int read_file(const char *path, Buf *b)
{
    wchar_t *w = NULL;
    FILE *f;
    char tmp[4096];
    size_t n;
    if (utf8_to_utf16(path, &w)) return -1;
    f = _wfopen(w, L"rb");
    free(w);
    if (!f) return -1;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (buf_append(b, tmp, n)) { fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

void config_free(Config *c)
{
    if (!c) return;
    free(c->username); free(c->password); free(c->download_path);
    memset(c, 0, sizeof *c);
}

int config_load(const char *path, Config *c)
{
    Buf b;
    const char *u, *p;
    memset(c, 0, sizeof *c);
    if (buf_init(&b, 1024)) return -1;
    if (read_file(path, &b) == 0) {
        size_t off = 0;
        JDoc d;
        int k;
        if (b.len >= 3 && (unsigned char)b.data[0] == 0xEF &&
            (unsigned char)b.data[1] == 0xBB && (unsigned char)b.data[2] == 0xBF) off = 3;
        memset(&d, 0, sizeof d);
        if (json_parse(&d, b.data + off, b.len - off) < 0 || d.n == 0) {
            json_free(&d); buf_free(&b); return -2;   /* 配置文件解析失败 */
        }
        k = json_obj_get(&d, 0, "username");
        if (k >= 0) { Buf s; if (buf_init(&s, 64) == 0) { json_str(&d, k, &s); c->username = buf_take(&s); } }
        k = json_obj_get(&d, 0, "password");
        if (k >= 0) { Buf s; if (buf_init(&s, 64) == 0) { json_str(&d, k, &s); c->password = buf_take(&s); } }
        k = json_obj_get(&d, 0, "download_path");
        if (k >= 0) { Buf s; if (buf_init(&s, 64) == 0) { json_str(&d, k, &s); c->download_path = buf_take(&s); } }
        json_free(&d);
    }
    buf_free(&b);

    u = getenv("PAN123_USERNAME");
    p = getenv("PAN123_PASSWORD");
    if (u && p && *u && *p) {
        free(c->username); free(c->password);
        c->username = str_dup(u);
        c->password = str_dup(p);
    }
    if (!c->username || !*c->username || !c->password || !*c->password) return -3;
    return 0;
}

static int writable(const char *dir)
{
    Buf p;
    wchar_t *wd = NULL, *wp = NULL;
    FILE *f;
    int ok = 0;
    if (!dir || !*dir) return 0;
    if (utf8_to_utf16(dir, &wd)) return 0;
    CreateDirectoryW(wd, NULL);
    free(wd);
    if (buf_init(&p, 256)) return 0;
    if (buf_printf(&p, "%s\\~123pan.tmp", dir) == 0 &&
        utf8_to_utf16(p.data, &wp) == 0) {
        f = _wfopen(wp, L"wb");
        if (f) {
            fwrite("ok", 1, 2, f);
            fclose(f);
            DeleteFileW(wp);
            ok = 1;
        }
        free(wp);
    }
    buf_free(&p);
    return ok;
}

char *default_download_dir(const char *configured)
{
    char drives[26];
    size_t nd = 0, i;
    char cands[6][1024];
    size_t nc = 0;
    int n;
    const char *home;
    if (configured && *configured && writable(configured)) return str_dup(configured);
    for (i = 0; i < 26; i++) {
        wchar_t root[4];
        root[0] = (wchar_t)('A' + i); root[1] = L':'; root[2] = L'\\'; root[3] = 0;
        if (GetDriveTypeW(root) == DRIVE_FIXED) drives[nd++] = (char)('A' + i);
    }
    if (nd >= 2) {
        n = snprintf(cands[nc], sizeof cands[0], "%c:\\downloads", drives[1]);
        if (n > 0 && (size_t)n < sizeof cands[0]) nc++;
    }
    if (nd >= 1) {
        n = snprintf(cands[nc], sizeof cands[0], "%c:\\downloads", drives[0]);
        if (n > 0 && (size_t)n < sizeof cands[0]) nc++;
    }
    home = getenv("USERPROFILE");
    if (home) {
        n = snprintf(cands[nc], sizeof cands[0], "%s\\downloads", home);
        if (n > 0 && (size_t)n < sizeof cands[0]) nc++;
    }
    n = snprintf(cands[nc], sizeof cands[0], "downloads");
    if (n > 0 && (size_t)n < sizeof cands[0]) nc++;
    for (i = 0; i < nc; i++) if (writable(cands[i])) return str_dup(cands[i]);
    return str_dup(".");
}
