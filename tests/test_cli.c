#include "pan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, name) do { if (!(c)) { printf("FAIL %s :: %s\n", name, resolve_error()); fail = 1; } else { printf("PASS %s\n", name); } } while (0)

int main(void)
{
    Config cfg;
    Client c;
    Resolver r;
    Options o;
    FileList hits;
    char *display = NULL;
    char *argv[] = { "prog", "download", "OS/3", "--out", "D:\\tmp", "--all", "--verbose" };
    char *sel[] = { "file.bin" };
    char dir[512], target[600];
    size_t n = 0;
    char **v;
    HttpResponse q = {0};
    int fail = 0, rc;
    const char *tmp = getenv("TEMP") ? getenv("TEMP") : ".";

    _putenv("PAN123_API_BASE=http://127.0.0.1:18080");
    _putenv("PAN123_LOGIN_BASE=http://127.0.0.1:18080");
    snprintf(dir, sizeof dir, "%s\\pan_cli_test", tmp);
    snprintf(target, sizeof target, "%s\\file.bin", dir);
    remove(target);

    /* 参数解析 */
    memset(&o, 0, sizeof o);
    CHECK(parse_args(7, argv, &o) == 0, "parse_args");
    CHECK(strcmp(o.command, "download") == 0 && o.nargs == 1 &&
          strcmp(o.args[0], "OS/3") == 0, "command + selector");
    CHECK(o.out && strcmp(o.out, "D:\\tmp") == 0 && o.all && o.verbose, "flags");
    options_free(&o);

    /* 路径分段与选择器 */
    v = split_selector("design soft/OS\\win*.iso", &n);
    CHECK(n == 3 && strcmp(v[0], "design soft") == 0 &&
          strcmp(v[2], "win*.iso") == 0, "split_selector");
    split_selector_free(v, n);
    CHECK(is_index("12") == 12 && is_index("a") == 0, "is_index");
    CHECK(glob_match("win*.iso", "WIN11_A.iso") == 1, "glob_match");
    CHECK(glob_match("win?.iso", "win1.iso") == 1, "glob_match ?");
    {
        char *sn = safe_name("a:b/c*d?.exe");
        CHECK(sn && strpbrk(sn, "\\/:*?\"<>|") == NULL, "safe_name");
        free(sn);
    }

    /* mock 链路 */
    memset(&cfg, 0, sizeof cfg);
    cfg.username = str_dup("u");
    cfg.password = str_dup("p");
    memset(&c, 0, sizeof c);
    client_init(&c, &cfg, 0);
    CHECK(api_login(&c) == 0, "login");
    r.c = &c;

    filelist_init(&hits);
    rc = resolve_select(&r, "sub/inner.bin", "", 0, 0, &hits, &display);
    CHECK(rc == 0 && hits.n == 1 && strcmp(hits.v[0].file_name, "inner.bin") == 0,
          "resolve sub/inner.bin");
    free(display); display = NULL;
    filelist_free(&hits);

    filelist_init(&hits);
    rc = resolve_select(&r, "1", "", 0, 0, &hits, &display);
    CHECK(rc != 0, "index 1 is folder -> needs --dir");
    free(display); display = NULL;
    filelist_free(&hits);

    filelist_init(&hits);
    rc = resolve_select(&r, "1", "", 1, 0, &hits, &display);
    CHECK(rc == 0 && hits.n == 1 && hits.v[0].type == 1, "--dir selects folder");
    free(display); display = NULL;
    filelist_free(&hits);

    filelist_init(&hits);
    rc = resolve_select(&r, "*.bin", "", 0, 0, &hits, &display);
    CHECK(rc == 0 && hits.n == 1, "glob selector");
    free(display); display = NULL;
    filelist_free(&hits);

    /* CLI list / download */
    memset(&o, 0, sizeof o);
    o.command = "list";
    o.in = "sub";
    CHECK(run_cli(&c, &o, dir) == 0, "run_cli list");

    memset(&o, 0, sizeof o);
    o.command = "download";
    o.args = sel;
    o.nargs = 1;
    CHECK(run_cli(&c, &o, dir) == 0, "run_cli download");
    {
        FILE *f = fopen(target, "rb");
        long sz = 0;
        if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
        CHECK(sz == 10240, "cli download wrote file");
    }

    client_free(&c);
    config_free(&cfg);
    remove(target);
    http_simple("GET", "http://127.0.0.1:18080/quit", NULL, NULL, 0, NULL, 0, 1, &q);
    http_response_free(&q);
    return fail ? 1 : 0;
}
