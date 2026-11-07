#include "pan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_arg_err[256];

const char *args_error(void) { return g_arg_err[0] ? g_arg_err : "参数错误"; }

int parse_args(int argc, char **argv, Options *o)
{
    size_t cap = 8;
    int i;
    memset(o, 0, sizeof *o);
    g_arg_err[0] = 0;
    o->args = (char **)xmalloc(cap * sizeof(char *));
    if (!o->args) return -1;
    for (i = 1; i < argc; i++) {
        char *a = argv[i], *eq, *val;
        char key[64];
        size_t klen;
        if (a[0] != '-') {
            if (!o->command) o->command = a;
            else {
                if (o->nargs + 1 >= cap) {
                    char **nv = (char **)realloc(o->args, cap * 2 * sizeof(char *));
                    if (!nv) return -1;
                    o->args = nv; cap *= 2;
                }
                o->args[o->nargs++] = a;
            }
            continue;
        }
        eq = strchr(a, '=');
        klen = eq ? (size_t)(eq - a) : strlen(a);
        if (klen >= sizeof key) { snprintf(g_arg_err, sizeof g_arg_err, "未知参数 %s", a); return -1; }
        memcpy(key, a, klen);
        key[klen] = 0;
        val = eq ? eq + 1 : NULL;
        if (strcmp(key, "--in") == 0 || strcmp(key, "--out") == 0 ||
            strcmp(key, "--config") == 0) {
            if (!val) {
                if (i + 1 >= argc) {
                    snprintf(g_arg_err, sizeof g_arg_err, "%s 缺少参数", key);
                    return -1;
                }
                val = argv[++i];
            }
            if (strcmp(key, "--in") == 0) o->in = val;
            else if (strcmp(key, "--out") == 0) o->out = val;
            else o->config = val;
        } else if (strcmp(key, "--all") == 0) o->all = 1;
        else if (strcmp(key, "--dir") == 0) o->dir = 1;
        else if (strcmp(key, "--yes") == 0) o->yes = 1;
        else if (strcmp(key, "--verbose") == 0) o->verbose = 1;
        else if (strcmp(key, "-h") == 0 || strcmp(key, "--help") == 0) o->help = 1;
        else { snprintf(g_arg_err, sizeof g_arg_err, "未知参数 %s", a); return -1; }
    }
    return 0;
}

void options_free(Options *o)
{
    if (!o) return;
    free(o->args);
    memset(o, 0, sizeof *o);
}

void print_help(void)
{
    out("用法:\n"
        "  123panclient_C.exe                          交互菜单\n"
        "  123panclient_C.exe list [--in \"OS\"]         列目录\n"
        "  123panclient_C.exe download <选择器> [...]  一行命令下载\n"
        "---\n"
        "选择器:\n"
        "  7                       根目录第 7 项\n"
        "  cad2014.1.7z            根目录按文件名（支持模糊）\n"
        "  \"OS/3\"                  OS 目录第 3 项\n"
        "  \"design soft/xx.exe\"    含空格、多层的子目录文件\n"
        "  \"OS/win*.iso\"           通配（命中多个需 --all）\n"
        "---\n"
        "选项:\n"
        "  --in \"OS\"               指定目录（等价于路径前缀）\n"
        "  --out \"D:\\xxx\"          自定义输出目录\n"
        "  --all                   多命中全部下载\n"
        "  --dir --yes             下载文件夹(zip)，--yes 为二次确认\n"
        "  --config <path>         指定 config.json\n"
        "  --verbose               附加信息（永不输出签名 URL）\n");
}

static char *pad(const char *s, int width)
{
    int n = width - utf8_width(s);
    Buf b;
    if (buf_init(&b, 128)) return NULL;
    buf_append_cstr(&b, s ? s : "");
    if (n > 0) for (int i = 0; i < n; i++) buf_append(&b, " ", 1);
    return buf_take(&b);
}

void print_list(const char *display, const FileList *l)
{
    size_t i;
    char *h1 = pad("序号", 5), *h2 = pad("类型", 8), *h3 = pad("大小", 12);
    const char *line = "------------------------------------------------------------------------------";
    out("当前目录: %s (共 %zu 项)\n", dir_label(display), l->n);
    out("%s\n", line);
    out("%s %s %s 文件名\n", h1 ? h1 : "", h2 ? h2 : "", h3 ? h3 : "");
    out("%s\n", line);
    free(h1); free(h2); free(h3);
    for (i = 0; i < l->n; i++) {
        const FileItem *it = &l->v[i];
        const char *kind = (it->type == 1) ? "文件夹" : "文件";
        char *size = it->size ? human(it->size) : str_dup("-");
        char *idx = NULL, *p1, *p2, *p3;
        char num[32];
        snprintf(num, sizeof num, "%zu", i + 1);
        idx = pad(num, 5);
        p1 = pad(kind, 8);
        p2 = pad(size ? size : "-", 12);
        p3 = pad(it->file_name, 0);
        out("%s %s %s %s\n", idx ? idx : "", p1 ? p1 : "", p2 ? p2 : "", p3 ? p3 : "");
        free(idx); free(p1); free(p2); free(p3);
        free(size);
    }
}

static void log_fail(const char *msg)
{
    Buf b;
    if (buf_init(&b, 256) == 0) {
        buf_printf(&b, "%s: %s", msg, resolve_error());
        log_only("ERROR", b.data);
        buf_free(&b);
    }
}

int run_cli(Client *c, Options *o, const char *outdir)
{
    Resolver r;
    size_t i, j;
    r.c = c;
    if (!o->command) { print_help(); return 0; }
    if (strcmp(o->command, "help") == 0) { print_help(); return 0; }
    if (strcmp(o->command, "list") == 0) {
        size_t n;
        char **segs = split_selector(o->in, &n);
        char *parent = NULL, *display = NULL;
        FileList items;
        int rc;
        filelist_init(&items);
        rc = resolve_dir(&r, segs, n, &parent, &display, &items);
        if (rc) {
            out("失败: %s\n", resolve_error());
            log_fail("列目录失败");
        } else {
            print_list(display, &items);
        }
        free(parent); free(display);
        filelist_free(&items);
        split_selector_free(segs, n);
        return rc ? 1 : 0;
    }
    if (strcmp(o->command, "download") == 0) {
        int ok = 1;
        if (o->nargs == 0) { out("失败: 请给出序号或文件名\n"); return 2; }
        if (o->dir && !o->yes) {
            out("警告: 文件夹会打包成 zip 下载，可能非常大。确认请加 --yes。\n");
            return 1;
        }
        for (i = 0; i < o->nargs; i++) {
            FileList hits;
            char *display = NULL;
            filelist_init(&hits);
            if (resolve_select(&r, o->args[i], o->in, o->dir, o->all, &hits, &display)) {
                out("失败: %s\n", resolve_error());
                log_fail("解析失败");
                ok = 0;
                free(display);
                filelist_free(&hits);
                continue;
            }
            for (j = 0; j < hits.n; j++) {
                FileItem *it = &hits.v[j];
                char *name = safe_name(it->file_name);
                char *hs = human(it->size);
                DownloadResult res;
                Buf nb;
                buf_init(&nb, 128);
                buf_append_cstr(&nb, name ? name : "");
                if (it->type == 1) {
                    buf_append_cstr(&nb, ".zip");
                    out("警告: 正在打包文件夹 %s (%s)\n", it->file_name, hs ? hs : "");
                }
                {
                    char *disp = buf_take(&nb);
                    out("开始下载: %s (%s) -> %s\n", disp ? disp : "", hs ? hs : "", outdir);
                    memset(&res, 0, sizeof res);
                    if (download_file(c, it, outdir, &res)) {
                        out("失败: %s — %s\n", disp ? disp : "", download_error());
                        log_fail("下载失败");
                        ok = 0;
                    } else if (res.skipped) {
                        out("跳过: %s（已存在且大小一致）\n", disp ? disp : "");
                    } else {
                        char *hb = human(res.bytes);
                        out("完成: %s (%s) -> %s\n", disp ? disp : "", hb ? hb : "", res.path);
                        free(hb);
                    }
                    free(disp);
                }
                download_result_free(&res);
                free(name); free(hs);
            }
            free(display);
            filelist_free(&hits);
        }
        return ok ? 0 : 1;
    }
    out("未知命令: %s\n", o->command);
    print_help();
    return 2;
}
