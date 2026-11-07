#include "pan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static char g_res_err[512];

const char *resolve_error(void) { return g_res_err[0] ? g_res_err : "解析失败"; }

static void res_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_res_err, sizeof g_res_err, fmt, ap);
    va_end(ap);
}

int is_index(const char *s)
{
    long n;
    char *e;
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return 0;
    n = strtol(s, &e, 10);
    return (n > 0 && *e == 0) ? (int)n : 0;
}

int has_glob(const char *s) { return s && (strchr(s, '*') || strchr(s, '?') || strchr(s, '[')); }

char *join_path(const char *base, const char *name)
{
    Buf b;
    if (buf_init(&b, 128)) return NULL;
    if (base && *base) { buf_append_cstr(&b, base); buf_append_cstr(&b, "/"); }
    buf_append_cstr(&b, name ? name : "");
    return buf_take(&b);
}

const char *dir_label(const char *s) { return (s && *s) ? s : "根目录"; }

char **split_selector(const char *s, size_t *n)
{
    char **v;
    size_t cap = 8, cnt = 0, start;
    const char *p;
    *n = 0;
    if (!s) { v = (char **)xmalloc(sizeof(char *)); v[0] = NULL; return v; }
    v = (char **)xmalloc(cap * sizeof(char *));
    if (!v) return NULL;
    p = s;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        start = (size_t)(p - s);
        while (*p && *p != '/' && *p != '\\') p++;
        if (cnt + 1 >= cap) {
            char **nv = (char **)realloc(v, (cap * 2) * sizeof(char *));
            if (!nv) { split_selector_free(v, cnt); return NULL; }
            v = nv; cap *= 2;
        }
        v[cnt] = str_dup_n(s + start, (size_t)(p - s) - start);
        if (!v[cnt]) { split_selector_free(v, cnt); return NULL; }
        cnt++;
    }
    v[cnt] = NULL;
    *n = cnt;
    return v;
}

void split_selector_free(char **v, size_t n)
{
    size_t i;
    if (!v) return;
    for (i = 0; i < n; i++) free(v[i]);
    free(v);
}

static char *lower(const char *s)
{
    size_t i, n = strlen(s);
    char *p = (char *)xmalloc(n + 1);
    if (!p) return NULL;
    for (i = 0; i < n; i++)
        p[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
    p[n] = 0;
    return p;
}

static int class_match(const char *p, char c, const char **end)
{
    int neg = 0, hit = 0;
    if (*p == '^' || *p == '!') { neg = 1; p++; }
    while (*p && *p != ']') {
        if (p[1] == '-' && p[2] && p[2] != ']') {
            if (c >= *p && c <= p[2]) hit = 1;
            p += 3;
        } else {
            if (c == *p) hit = 1;
            p++;
        }
    }
    while (*p && *p != ']') p++;
    *end = *p == ']' ? p + 1 : p;
    return neg ? !hit : hit;
}

static int glob_rec(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            const char *t;
            p++;
            while (*p == '*') p++;
            if (!*p) return 1;
            for (t = s; ; t++) {
                if (glob_rec(p, t)) return 1;
                if (!*t) break;
            }
            return 0;
        }
        if (*p == '?') { if (!*s) return 0; p++; s++; continue; }
        if (*p == '[') {
            const char *e;
            if (!*s || !class_match(p + 1, *s, &e)) return 0;
            p = e; s++; continue;
        }
        if (*p == '\\' && p[1]) { p++; if (*s != *p) return 0; p++; s++; continue; }
        if (*s != *p) return 0;
        p++; s++;
    }
    return *s == 0;
}

int glob_match(const char *pat, const char *name)
{
    char *lp = lower(pat), *ln = lower(name);
    int ok;
    if (!lp || !ln) { free(lp); free(ln); return 0; }
    ok = glob_rec(lp, ln);
    free(lp); free(ln);
    return ok;
}

static FileItem clone_item(const FileItem *src)
{
    FileItem d;
    memset(&d, 0, sizeof d);
    d.file_name = str_dup(src->file_name);
    d.size = src->size;
    d.type = src->type;
    d.file_id = str_dup(src->file_id);
    d.id = str_dup(src->id);
    d.etag = str_dup(src->etag);
    d.s3key_flag = str_dup(src->s3key_flag);
    return d;
}

static int pick_one(const FileList *items, const char *sel, int want_dir,
                    int allow_all, const char *display, FileList *hits)
{
    int n, i;
    char *low;
    filelist_init(hits);
    n = is_index(sel);
    if (n) {
        const FileItem *it;
        if ((size_t)n > items->n) {
            res_err("序号 %d 超出范围（共 %zu 项）", n, items->n);
            return -1;
        }
        it = &items->v[n - 1];
        if ((it->type == 1) && !want_dir) {
            res_err("第 %d 项是文件夹，下载文件夹请加 --dir", n);
            return -1;
        }
        if ((it->type != 1) && want_dir) {
            res_err("第 %d 项不是文件夹", n);
            return -1;
        }
        return filelist_push(hits, clone_item(it));
    }
    low = lower(sel);
    if (!low) return -1;
    if (has_glob(sel)) {
        for (i = 0; i < (int)items->n; i++) {
            if ((items->v[i].type == 1) != (want_dir != 0)) continue;
            if (glob_match(low, items->v[i].file_name))
                if (filelist_push(hits, clone_item(&items->v[i]))) { free(low); return -1; }
        }
    } else {
        int exact = 0;
        for (i = 0; i < (int)items->n; i++) {
            const char *nm = items->v[i].file_name;
            char *ln;
            if ((items->v[i].type == 1) != (want_dir != 0)) continue;
            if (strcmp(nm, sel) == 0) {
                filelist_free(hits);
                filelist_init(hits);
                if (filelist_push(hits, clone_item(&items->v[i]))) { free(low); return -1; }
                exact = 1;
                break;
            }
            ln = lower(nm);
            if (ln && strcmp(ln, low) == 0) {
                if (filelist_push(hits, clone_item(&items->v[i]))) { free(ln); free(low); return -1; }
            }
            free(ln);
        }
        if (!exact && hits->n == 0) {
            for (i = 0; i < (int)items->n; i++) {
                char *ln;
                if ((items->v[i].type == 1) != (want_dir != 0)) continue;
                ln = lower(items->v[i].file_name);
                if (ln && strstr(ln, low)) {
                    if (filelist_push(hits, clone_item(&items->v[i]))) { free(ln); free(low); return -1; }
                }
                free(ln);
            }
        }
    }
    free(low);
    if (hits->n == 0) {
        res_err("未找到 \"%s\"（当前目录: %s）", sel, dir_label(display));
        return -1;
    }
    if (hits->n > 1 && !allow_all) {
        Buf b;
        size_t k;
        if (buf_init(&b, 256)) return -1;
        for (k = 0; k < hits->n && k < 8; k++) {
            char *jp = join_path(display, hits->v[k].file_name);
            if (k) buf_append_cstr(&b, ", ");
            buf_append_cstr(&b, jp ? jp : "");
            free(jp);
        }
        if (hits->n > 8) buf_append_cstr(&b, ", ...");
        res_err("\"%s\" 匹配到 %zu 项: %s；加 --all 全部下载", sel, hits->n, b.data);
        buf_free(&b);
        return -1;
    }
    return 0;
}

int resolve_dir(Resolver *r, char **segs, size_t n, char **parent_out,
                char **display_out, FileList *items)
{
    char *parent = str_dup("0"), *display = str_dup("");
    size_t i;
    *parent_out = *display_out = NULL;
    filelist_init(items);
    for (i = 0; i < n; i++) {
        FileList cur, hits;
        char *np;
        filelist_init(&cur);
        if (api_list(r->c, parent, &cur)) {
            res_err("列目录失败: %s", api_error());
            goto fail;
        }
        if (pick_one(&cur, segs[i], 1, 0, display, &hits)) {
            char *tmp = NULL;
            Buf e;
            if (buf_init(&e, 512) == 0) {
                buf_printf(&e, "进入目录 \"%s\" 失败: %s", segs[i], resolve_error());
                tmp = buf_take(&e);
            }
            res_err("%s", tmp ? tmp : "进入目录失败");
            free(tmp);
            filelist_free(&cur);
            goto fail;
        }
        np = str_dup(hits.v[0].id);
        {
            char *nd = join_path(display, hits.v[0].file_name);
            free(display);
            display = nd;
        }
        free(parent);
        parent = np;
        filelist_free(&hits);
        filelist_free(&cur);
    }
    if (api_list(r->c, parent, items)) {
        res_err("列目录失败: %s", api_error());
        goto fail;
    }
    *parent_out = parent;
    *display_out = display;
    return 0;
fail:
    free(parent); free(display);
    filelist_free(items);
    return -1;
}

int resolve_select(Resolver *r, const char *selector, const char *in_dir,
                   int allow_dir, int match_all, FileList *hits, char **display_out)
{
    size_t nsel = 0, nin = 0, i, nsegs;
    char **sel = split_selector(selector, &nsel);
    char **din = split_selector(in_dir, &nin);
    char **segs;
    char *parent = NULL, *display = NULL;
    FileList items;
    int ret = -1;
    *display_out = NULL;
    filelist_init(hits);
    filelist_init(&items);
    if (!sel || !din || nsel == 0) { res_err("请给出序号或文件名"); goto done; }
    nsegs = nin + nsel - 1;
    segs = (char **)xmalloc((nsegs + 1) * sizeof(char *));
    if (!segs) goto done;
    for (i = 0; i < nin; i++) segs[i] = din[i];
    for (i = 0; i + 1 < nsel; i++) segs[nin + i] = sel[i];
    segs[nsegs] = NULL;
    if (resolve_dir(r, segs, nsegs, &parent, &display, &items)) { free(segs); goto done; }
    free(segs);
    ret = pick_one(&items, sel[nsel - 1], allow_dir, match_all, display, hits);
    if (ret == 0) { *display_out = display; display = NULL; }
done:
    free(parent);
    free(display);
    filelist_free(&items);
    split_selector_free(sel, nsel);
    split_selector_free(din, nin);
    return ret;
}
