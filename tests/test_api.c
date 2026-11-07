#include "pan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, name) do { if (!(c)) { printf("FAIL %s :: %s\n", name, api_error()); fail = 1; } else { printf("PASS %s\n", name); } } while (0)

int main(void)
{
    Config cfg;
    Client c;
    FileList l, l2, empty;
    char *url1 = NULL, *url2 = NULL, *key = NULL;
    HttpResponse r = {0};
    int fail = 0;

    _putenv("PAN123_API_BASE=http://127.0.0.1:18080");
    _putenv("PAN123_LOGIN_BASE=http://127.0.0.1:18080");
    memset(&cfg, 0, sizeof cfg);
    cfg.username = str_dup("u");
    cfg.password = str_dup("p");
    filelist_init(&l); filelist_init(&l2); filelist_init(&empty);
    memset(&c, 0, sizeof c);

    CHECK(client_init(&c, &cfg, 0) == 0, "client_init");
    CHECK(strlen(c.login_uuid) == 32, "login_uuid 32 hex");
    CHECK(api_login(&c) == 0, "api_login");
    CHECK(c.token && strcmp(c.token, "mock-token-abcdef") == 0, "token");

    CHECK(api_list(&c, "0", &l) == 0, "api_list root");
    CHECK(l.n == 2, "root has 2 items");
    if (l.n == 2) {
        CHECK(strcmp(l.v[0].file_name, "sub") == 0 && l.v[0].type == 1, "dir item");
        CHECK(l.v[0].id && strcmp(l.v[0].id, "100") == 0, "string FileId -> id");
        CHECK(l.v[0].file_id && strcmp(l.v[0].file_id, "\"100\"") == 0, "raw string FileId");
        CHECK(strcmp(l.v[1].file_name, "file.bin") == 0 && l.v[1].type == 0, "file item");
        CHECK(l.v[1].size == 10240, "size");
        CHECK(l.v[1].file_id && strcmp(l.v[1].file_id, "200") == 0, "raw numeric FileId");
        CHECK(l.v[1].s3key_flag && strcmp(l.v[1].s3key_flag, "\"0\"") == 0, "raw string S3KeyFlag");
        CHECK(api_download_url(&c, &l.v[1], &url1) == 0, "download_url via Location");
        CHECK(url1 && strstr(url1, "/cdn") != NULL, "Location is final CDN url");
        CHECK(api_download_url(&c, &l.v[1], &url2) == 0, "download_url via href fallback");
        CHECK(url2 && strstr(url2, "/cdn") != NULL, "href fallback works");
        CHECK(api_trash(&c, &l.v[1]) == 0, "api_trash");
    }
    CHECK(api_list(&c, "100", &l2) == 0 && l2.n == 1, "api_list sub dir");
    CHECK(api_mkdir(&c, "0", "newdir") == 0, "api_mkdir");
    CHECK(api_share(&c, "100,200", &key) == 0, "api_share");
    CHECK(key && strcmp(key, "MOCKKEY") == 0, "ShareKey");

    free(url1); free(url2); free(key);
    filelist_free(&l); filelist_free(&l2); filelist_free(&empty);
    client_free(&c);
    config_free(&cfg);

    /* 通知 mock 退出 */
    http_simple("GET", "http://127.0.0.1:18080/quit", NULL, NULL, 0, NULL, 0, 1, &r);
    http_response_free(&r);
    return fail ? 1 : 0;
}
