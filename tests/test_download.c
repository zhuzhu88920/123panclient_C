#include "pan.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, name) do { if (!(c)) { printf("FAIL %s :: %s\n", name, download_error()); fail = 1; } else { printf("PASS %s\n", name); } } while (0)

static void expect_bytes(unsigned char *p, size_t n)
{
    static const char pat[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)pat[i % 16];
}

static int file_is(const char *path, const unsigned char *want, size_t n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    size_t got;
    int ok = 0;
    if (!f) return 0;
    b = (unsigned char *)malloc(n + 1);
    if (!b) { fclose(f); return 0; }
    got = fread(b, 1, n + 1, f);
    fclose(f);
    ok = (got == n && memcmp(b, want, n) == 0);
    free(b);
    return ok;
}

static int file_len_is(const char *path, size_t n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fclose(f);
    return sz == (long)n;
}

int main(void)
{
    Config cfg;
    Client c;
    FileList l;
    DownloadResult res;
    HttpResponse q = {0};
    unsigned char want[10240];
    char dir[512], target[600], part[600], zip[600], zip_part[600];
    FILE *f;
    FileItem bad;
    int fail = 0;
    const char *tmp = getenv("TEMP") ? getenv("TEMP") : ".";

    _putenv("PAN123_API_BASE=http://127.0.0.1:18080");
    _putenv("PAN123_LOGIN_BASE=http://127.0.0.1:18080");
    expect_bytes(want, sizeof want);
    snprintf(dir, sizeof dir, "%s\\pan_dl_test", tmp);
    snprintf(target, sizeof target, "%s\\file.bin", dir);
    snprintf(part, sizeof part, "%s\\file.bin.part", dir);
    snprintf(zip, sizeof zip, "%s\\sub.zip", dir);
    snprintf(zip_part, sizeof zip_part, "%s\\sub.zip.part", dir);
    remove(target); remove(part); remove(zip); remove(zip_part);

    memset(&cfg, 0, sizeof cfg);
    cfg.username = str_dup("u");
    cfg.password = str_dup("p");
    memset(&c, 0, sizeof c);
    filelist_init(&l);
    client_init(&c, &cfg, 0);
    CHECK(api_login(&c) == 0, "login");
    CHECK(api_list(&c, "0", &l) == 0 && l.n == 2, "list root");
    if (l.n != 2) return 1;

    /* 1. 全新下载 */
    memset(&res, 0, sizeof res);
    CHECK(download_file(&c, &l.v[1], dir, &res) == 0, "fresh download");
    CHECK(!res.skipped && res.bytes == 10240, "fresh result");
    CHECK(file_is(target, want, sizeof want), "fresh content matches");
    CHECK(GetFileAttributesA(part) == 0xFFFFFFFF, ".part renamed away");
    download_result_free(&res);

    /* 2. 已完成则跳过 */
    memset(&res, 0, sizeof res);
    CHECK(download_file(&c, &l.v[1], dir, &res) == 0 && res.skipped, "skip existing");

    /* 3. .part 续传 */
    remove(target);
    f = fopen(part, "wb");
    if (!f) { printf("FAIL write .part\n"); return 1; }
    fwrite(want, 1, 4000, f);
    fclose(f);
    memset(&res, 0, sizeof res);
    CHECK(download_file(&c, &l.v[1], dir, &res) == 0, "resume download");
    CHECK(res.bytes == 10240, "resume total bytes");
    CHECK(file_is(target, want, sizeof want), "resumed content matches");
    download_result_free(&res);

    /* 4. 大小不符保留 .part */
    memset(&bad, 0, sizeof bad);
    bad.file_name = str_dup("bad.bin");
    bad.file_id = str_dup("999");
    bad.id = str_dup("999");
    bad.etag = str_dup("e1");
    bad.s3key_flag = str_dup("0");
    bad.size = 99999;
    bad.type = 0;
    memset(&res, 0, sizeof res);
    CHECK(download_file(&c, &bad, dir, &res) != 0, "size mismatch rejected");
    {
        char bp[600];
        snprintf(bp, sizeof bp, "%s\\bad.bin.part", dir);
        CHECK(file_len_is(bp, 10240), ".part kept on mismatch");
        remove(bp);
    }
    fileitem_free(&bad);
    download_result_free(&res);

    /* 5. 文件夹打包成 zip */
    memset(&res, 0, sizeof res);
    CHECK(download_file(&c, &l.v[0], dir, &res) == 0, "folder zip download");
    CHECK(file_is(zip, want, sizeof want), "zip content matches");
    CHECK(GetFileAttributesA(zip_part) == 0xFFFFFFFF, "zip .part renamed away");
    download_result_free(&res);

    filelist_free(&l);
    client_free(&c);
    config_free(&cfg);
    remove(target); remove(zip);
    http_simple("GET", "http://127.0.0.1:18080/quit", NULL, NULL, 0, NULL, 0, 1, &q);
    http_response_free(&q);
    return fail ? 1 : 0;
}
