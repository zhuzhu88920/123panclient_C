#include "pan.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static char g_dl_err[512];

const char *download_error(void) { return g_dl_err[0] ? g_dl_err : "下载失败"; }

static void dlerr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_dl_err, sizeof g_dl_err, fmt, ap);
    va_end(ap);
}

char *safe_name(const char *s)
{
    Buf b;
    char *t, *start, *end;
    const char *p;
    if (buf_init(&b, 128)) return NULL;
    for (p = s ? s : ""; *p; p++) {
        char c = *p;
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        buf_append(&b, &c, 1);
    }
    t = buf_take(&b);
    if (!t) return NULL;
    start = t;
    while (*start && is_space_ch(*start)) start++;
    end = t + strlen(t);
    while (end > start && is_space_ch(end[-1])) end--;
    *end = 0;
    if (start != t) memmove(t, start, (size_t)(end - start) + 1);
    return t;
}

static int ensure_dir(const char *dir)
{
    Buf p;
    const char *q;
    wchar_t *w = NULL;
    if (!dir || !*dir) return -1;
    if (buf_init(&p, 256)) return -1;
    for (q = dir; *q; q++) {
        buf_append(&p, q, 1);
        if (*q == '\\' || *q == '/') {
            w = NULL;
            if (utf8_to_utf16(p.data, &w) == 0) { CreateDirectoryW(w, NULL); free(w); }
        }
    }
    w = NULL;
    if (utf8_to_utf16(p.data, &w) == 0) { CreateDirectoryW(w, NULL); free(w); }
    buf_free(&p);
    return 0;
}

/* 返回 1 表示是普通文件并给出大小；目录或不存在返回 0 */
static int file_size(const char *path, int64_t *sz)
{
    wchar_t *w = NULL;
    WIN32_FILE_ATTRIBUTE_DATA d;
    BOOL ok;
    if (utf8_to_utf16(path, &w)) return 0;
    ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    *sz = (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    return 1;
}

static int delete_file_u8(const char *path)
{
    wchar_t *w = NULL;
    int ok;
    if (utf8_to_utf16(path, &w)) return 0;
    ok = DeleteFileW(w) ? 1 : 0;
    free(w);
    return ok;
}

static int rename_file_u8(const char *from, const char *to)
{
    wchar_t *wf = NULL, *wt = NULL;
    int ok;
    if (utf8_to_utf16(from, &wf) || utf8_to_utf16(to, &wt)) {
        free(wf); free(wt); return 0;
    }
    ok = MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
    free(wf); free(wt);
    return ok;
}

static char *host_of(const char *url)
{
    const char *p = strstr(url, "://"), *e;
    if (!p) return str_dup("-");
    p += 3;
    e = p;
    while (*e && *e != '/') e++;
    return str_dup_n(p, (size_t)(e - p));
}

void download_result_free(DownloadResult *r)
{
    if (!r) return;
    free(r->path);
    memset(r, 0, sizeof *r);
}

int download_file(Client *c, const FileItem *it, const char *outdir, DownloadResult *r)
{
    Buf nb, target, part, msg;
    char *name = NULL, *final_name = NULL, *url = NULL, *host = NULL;
    char range[64], buf[65536];
    int64_t want, existing = 0, offset = 0, done, total, clen = -1, psz = 0;
    int status = 0, ret = -1, copy_err = 0;
    size_t got;
    uint64_t last_tick;
    FILE *f = NULL;
    wchar_t *wp = NULL;
    HttpStream *s = NULL;
    Progress *rep = NULL;

    memset(r, 0, sizeof *r);
    buf_init(&nb, 128); buf_init(&target, 256);
    buf_init(&part, 256); buf_init(&msg, 256);

    name = safe_name(it->file_name);
    if (!name) goto done;
    buf_append_cstr(&nb, name);
    if (it->type == 1) buf_append_cstr(&nb, ".zip");
    final_name = buf_take(&nb);
    if (!final_name) goto done;
    buf_printf(&target, "%s\\%s", outdir, final_name);
    buf_printf(&part, "%s.part", target.data);
    r->path = str_dup(target.data);
    want = it->size;

    if (want > 0 && file_size(target.data, &existing) && existing == want) {
        r->skipped = 1;
        r->bytes = want;
        ret = 0;
        goto done;
    }
    if (ensure_dir(outdir)) { dlerr("无法创建输出目录"); goto done; }
    if (file_size(part.data, &psz)) {
        if (want <= 0 || psz < want) offset = psz;
        else { delete_file_u8(part.data); offset = 0; }
    }
    if (api_download_url(c, it, &url)) {
        dlerr("%s", api_error());
        goto done;
    }
    if (c->verbose) {
        host = host_of(url);
        out("CDN: %s\n", host ? host : "-");
        free(host); host = NULL;
    }
    if (offset > 0) snprintf(range, sizeof range, "bytes=%lld-", (long long)offset);
    else range[0] = 0;
    if (http_stream_open(url, range[0] ? range : NULL, &s, &status, &clen)) {
        dlerr("%s", http_error());
        goto done;
    }
    if (status != 200 && status != 206) {
        dlerr("CDN 返回 HTTP %d", status);
        goto done;
    }
    if (offset > 0 && status == 200) offset = 0;   /* 服务端忽略 Range，干净重来 */
    total = want;
    if (clen > 0) total = offset + clen;

    if (utf8_to_utf16(part.data, &wp) == 0)
        f = _wfopen(wp, offset > 0 ? L"ab" : L"wb");
    free(wp); wp = NULL;
    if (!f) { dlerr("无法写入 .part 文件"); goto done; }

    done = offset;
    last_tick = tick_ms();
    rep = progress_new();
    for (;;) {
        if (http_stream_read(s, buf, sizeof buf, &got)) { copy_err = 1; break; }
        if (got > 0) {
            if (fwrite(buf, 1, got, f) != got) { copy_err = 2; break; }
            done += (int64_t)got;
            if (tick_ms() - last_tick >= 100) {
                progress_update(rep, done, total);
                last_tick = tick_ms();
            }
        }
        if (got == 0) break;
        if (g_interrupted) { copy_err = 3; break; }
    }
    progress_update(rep, done, total);
    progress_clear(rep);
    progress_free(rep);
    rep = NULL;
    if (f) { fclose(f); f = NULL; }

    if (copy_err) { dlerr("下载中断（已保留 .part 供续传）"); goto done; }
    if (g_interrupted) { dlerr("已中断（已保留 .part 供续传）"); goto done; }
    if (want > 0 && done != want) {
        dlerr("大小不符: %lld/%lld（已保留 .part 供续传）",
              (long long)done, (long long)want);
        goto done;
    }
    if (file_size(target.data, &existing)) delete_file_u8(target.data);
    if (!rename_file_u8(part.data, target.data)) {
        dlerr("完成重命名失败");
        goto done;
    }
    r->bytes = done;
    buf_printf(&msg, "下载完成 %s %lld 字节 -> %s",
               final_name, (long long)done, target.data);
    log_only("OK", msg.data);
    ret = 0;

done:
    if (rep) { progress_clear(rep); progress_free(rep); }
    if (f) fclose(f);
    if (s) http_stream_close(s);
    free(name); free(final_name); free(url); free(host);
    buf_free(&nb); buf_free(&target); buf_free(&part); buf_free(&msg);
    if (ret != 0) { free(r->path); r->path = NULL; r->bytes = 0; }
    return ret;
}
