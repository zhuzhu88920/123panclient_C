#include "pan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Client *c;
    const char *outdir;
    char *parent;
    char *display;
    char **stack;
    size_t nstack, capstack;
    FileList items;
} Menu;

static char *trim(char *s)
{
    char *e;
    while (*s && is_space_ch(*s)) s++;
    e = s + strlen(s);
    while (e > s && is_space_ch(e[-1])) e--;
    *e = 0;
    return s;
}

static char **fields(char *s, size_t *n)
{
    size_t cap = 16, cnt = 0;
    char **v = (char **)xmalloc(cap * sizeof(char *));
    char *p = s;
    *n = 0;
    if (!v) return NULL;
    while (*p) {
        char *start;
        while (*p && is_space_ch(*p)) p++;
        if (!*p) break;
        start = p;
        while (*p && !is_space_ch(*p)) p++;
        if (*p) { *p = 0; p++; }
        if (cnt == cap) {
            char **nv = (char **)realloc(v, cap * 2 * sizeof(char *));
            if (!nv) { free(v); return NULL; }
            v = nv; cap *= 2;
        }
        v[cnt++] = start;
    }
    *n = cnt;
    return v;
}

static void print_menu_help(void)
{
    out("命令:\n"
        "  ls / re            显示 / 刷新当前目录\n"
        "  <数字>             进入文件夹，或查看文件后输入 1 下载\n"
        "  download <n|路径>  下载（路径相对根目录）\n"
        "  cd <n|..|/|路径>   切换目录\n"
        "  link <n>           打印该文件的下载链接（含签名，注意保密）\n"
        "  log / reload       重新登录 / 重新登录并刷新\n"
        "  mkdir [名称]       新建文件夹（需确认）\n"
        "  delete [n]         删除到回收站（需确认）\n"
        "  share              创建分享链接（需确认）\n"
        "  upload             上传（本版未实现，请用网页端）\n"
        "  help / exit        帮助 / 退出\n");
}

static int confirm(const char *prompt)
{
    char *line = NULL, *t;
    int ok;
    out("%s (输入 1 继续): ", prompt);
    if (!read_line(&line)) return 0;
    t = trim(line);
    ok = strcmp(t, "1") == 0;
    free(line);
    return ok;
}

static void refresh(Menu *m)
{
    FileList next;
    filelist_init(&next);
    if (api_list(m->c, m->parent, &next)) {
        out("失败: %s\n", api_error());
        filelist_free(&next);
        return;
    }
    filelist_free(&m->items);
    m->items = next;
    print_list(m->display, &m->items);
}

static void push_parent(Menu *m, const char *p)
{
    if (m->nstack == m->capstack) {
        size_t cap = m->capstack ? m->capstack * 2 : 8;
        char **v = (char **)realloc(m->stack, cap * sizeof(char *));
        if (!v) return;
        m->stack = v; m->capstack = cap;
    }
    m->stack[m->nstack++] = str_dup(p);
}

static void enter(Menu *m, const char *id, const char *name)
{
    char *nd;
    push_parent(m, m->parent);
    free(m->parent);
    m->parent = str_dup(id);
    nd = join_path(m->display, name);
    free(m->display);
    m->display = nd;
    refresh(m);
}

static void up(Menu *m)
{
    char *last;
    if (m->nstack > 0) {
        m->nstack--;
        free(m->parent);
        m->parent = m->stack[m->nstack];
        m->stack[m->nstack] = NULL;
    } else {
        free(m->parent);
        m->parent = str_dup("0");
    }
    last = strrchr(m->display, '/');
    if (last) *last = 0;
    else { free(m->display); m->display = str_dup(""); }
    refresh(m);
}

static void download_one(Menu *m, const FileItem *it, int allow_dir)
{
    DownloadResult res;
    char *name = NULL, *hs = NULL;
    Buf nb;
    if (it->type == 1 && !allow_dir) {
        Buf p;
        char *msg;
        buf_init(&p, 256);
        hs = human(it->size);
        buf_printf(&p, "将打包下载文件夹 %s (%s)", it->file_name, hs ? hs : "");
        msg = buf_take(&p);
        if (!confirm(msg ? msg : "确认下载")) { free(msg); free(hs); return; }
        free(msg);
    }
    name = safe_name(it->file_name);
    buf_init(&nb, 128);
    buf_append_cstr(&nb, name ? name : "");
    if (it->type == 1) buf_append_cstr(&nb, ".zip");
    {
        char *disp = buf_take(&nb);
        char *size = human(it->size);
        out("开始下载: %s (%s) -> %s\n", disp ? disp : "", size ? size : "", m->outdir);
        free(size);
        memset(&res, 0, sizeof res);
        if (download_file(m->c, it, m->outdir, &res)) {
            out("失败: %s — %s\n", disp ? disp : "", download_error());
            log_only("ERROR", download_error());
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

static void menu_download(Menu *m, const char *arg)
{
    int n = is_index(arg);
    if (n) {
        if ((size_t)n > m->items.n) { out("输入错误\n"); return; }
        download_one(m, &m->items.v[n - 1], 1);
        return;
    }
    {
        Resolver r;
        FileList hits;
        char *display = NULL;
        size_t i;
        r.c = m->c;
        filelist_init(&hits);
        if (resolve_select(&r, arg, "", 1, 0, &hits, &display)) {
            out("失败: %s\n", resolve_error());
            free(display);
            filelist_free(&hits);
            return;
        }
        for (i = 0; i < hits.n; i++) download_one(m, &hits.v[i], 0);
        free(display);
        filelist_free(&hits);
    }
}

static void menu_cd(Menu *m, const char *arg)
{
    int n;
    size_t k, cnt = 0;
    char **segs;
    char *parent = NULL, *display = NULL;
    FileList items;
    Resolver r;
    if (strcmp(arg, "..") == 0) { up(m); return; }
    if (strcmp(arg, "/") == 0 || strcmp(arg, "\\") == 0) {
        for (k = 0; k < m->nstack; k++) free(m->stack[k]);
        m->nstack = 0;
        free(m->parent); m->parent = str_dup("0");
        free(m->display); m->display = str_dup("");
        refresh(m);
        return;
    }
    n = is_index(arg);
    if (n) {
        if ((size_t)n > m->items.n || m->items.v[n - 1].type != 1) {
            out("输入错误：不是文件夹\n");
            return;
        }
        enter(m, m->items.v[n - 1].id, m->items.v[n - 1].file_name);
        return;
    }
    r.c = m->c;
    segs = split_selector(arg, &cnt);
    filelist_init(&items);
    if (resolve_dir(&r, segs, cnt, &parent, &display, &items)) {
        out("失败: %s\n", resolve_error());
    } else {
        push_parent(m, "0");
        free(m->parent); m->parent = parent; parent = NULL;
        free(m->display); m->display = display; display = NULL;
        filelist_free(&m->items);
        m->items = items;
        filelist_init(&items);
        print_list(m->display, &m->items);
    }
    free(parent); free(display);
    filelist_free(&items);
    split_selector_free(segs, cnt);
}

static void menu_link(Menu *m, const char *arg)
{
    int n = is_index(arg);
    char *url = NULL;
    if (!n || (size_t)n > m->items.n) { out("输入错误\n"); return; }
    if (api_download_url(m->c, &m->items.v[n - 1], &url)) {
        out("失败: %s\n", api_error());
        return;
    }
    out("链接含临时签名，注意保密：\n%s\n", url);
    free(url);
}

static void menu_mkdir(Menu *m, const char *name)
{
    char *arg = NULL, *prompt = NULL;
    Buf p;
    if (!*name) {
        char *line = NULL;
        out("请输入目录名: ");
        if (!read_line(&line)) return;
        arg = str_dup(trim(line));
        free(line);
    } else {
        arg = str_dup(name);
    }
    if (!arg || !*arg) { free(arg); return; }
    if (buf_init(&p, 256) == 0) {
        buf_printf(&p, "确认新建文件夹 %s", arg);
        prompt = buf_take(&p);
    }
    if (!confirm(prompt ? prompt : "确认新建文件夹")) {
        free(prompt); free(arg); return;
    }
    free(prompt);
    if (api_mkdir(m->c, m->parent, arg)) out("创建失败: %s\n", api_error());
    else { out("创建成功: %s\n", arg); refresh(m); }
    free(arg);
}

static void menu_delete(Menu *m, const char *arg)
{
    char *own = NULL;
    int n;
    if (!*arg) {
        char *line = NULL;
        out("请输入要删除的编号: ");
        if (!read_line(&line)) return;
        own = str_dup(trim(line));
        free(line);
        arg = own;
    }
    n = is_index(arg);
    if (!n || (size_t)n > m->items.n) { out("输入错误\n"); free(own); return; }
    {
        FileItem *it = &m->items.v[n - 1];
        Buf p;
        char *prompt;
        buf_init(&p, 256);
        buf_printf(&p, "确认删除 %s", it->file_name);
        prompt = buf_take(&p);
        if (!confirm(prompt ? prompt : "确认删除")) { free(prompt); free(own); return; }
        free(prompt);
        if (api_trash(m->c, it)) {
            out("删除失败: %s\n", api_error());
        } else {
            Buf lb;
            out("已移入回收站: %s\n", it->file_name);
            if (buf_init(&lb, 256) == 0) {
                buf_printf(&lb, "删除到回收站: %s", it->file_name);
                log_only("WARN", lb.data);
                buf_free(&lb);
            }
            refresh(m);
        }
    }
    free(own);
}

static void menu_share(Menu *m)
{
    char *line = NULL, *t;
    char **f;
    size_t nf = 0, i;
    Buf ids, names;
    char *key = NULL;
    out("输入分享文件编号（多个用空格分隔）: ");
    if (!read_line(&line)) return;
    t = trim(line);
    f = fields(t, &nf);
    if (!f || nf == 0) { free(f); free(line); return; }
    buf_init(&ids, 128); buf_init(&names, 256);
    for (i = 0; i < nf; i++) {
        int n = is_index(f[i]);
        if (!n || (size_t)n > m->items.n) {
            out("输入错误: %s\n", f[i]);
            goto done;
        }
        if (i) { buf_append_cstr(&ids, ","); buf_append_cstr(&names, ", "); }
        buf_append_cstr(&ids, m->items.v[n - 1].id);
        buf_append_cstr(&names, m->items.v[n - 1].file_name);
    }
    {
        Buf p;
        char *prompt;
        buf_init(&p, 512);
        buf_printf(&p, "确认创建分享: %s", names.data);
        prompt = buf_take(&p);
        if (!confirm(prompt ? prompt : "确认创建分享")) { free(prompt); goto done; }
        free(prompt);
    }
    if (api_share(m->c, ids.data, &key)) {
        out("分享失败: %s\n", api_error());
    } else {
        out("分享链接: https://www.123pan.com/s/%s\n", key);
    }
done:
    free(key);
    buf_free(&ids); buf_free(&names);
    free(f); free(line);
}

static int dispatch(Menu *m, char *line)
{
    if (strcmp(line, "exit") == 0 || strcmp(line, "q") == 0) return 1;
    if (strcmp(line, "help") == 0) { print_menu_help(); return 0; }
    if (strcmp(line, "ls") == 0 || strcmp(line, "re") == 0) { refresh(m); return 0; }
    if (strcmp(line, "log") == 0 || strcmp(line, "reload") == 0) {
        if (api_login(m->c)) out("登录失败: %s\n", api_error());
        else { out("登录成功\n"); refresh(m); }
        return 0;
    }
    if (strcmp(line, "cd") == 0) { out("当前目录: %s\n", dir_label(m->display)); return 0; }
    if (strcmp(line, "..") == 0) { up(m); return 0; }
    if (strcmp(line, "share") == 0) { menu_share(m); return 0; }
    if (strcmp(line, "upload") == 0) {
        out("上传（分片/S3 私有协议）本版未实现，请用网页端上传。\n");
        return 0;
    }
    if (strcmp(line, "mkdir") == 0) { menu_mkdir(m, ""); return 0; }
    if (strncmp(line, "mkdir ", 6) == 0) { menu_mkdir(m, trim(line + 6)); return 0; }
    if (strcmp(line, "delete") == 0) { menu_delete(m, ""); return 0; }
    if (strncmp(line, "delete ", 7) == 0) { menu_delete(m, trim(line + 7)); return 0; }
    if (strncmp(line, "download ", 9) == 0) { menu_download(m, trim(line + 9)); return 0; }
    if (strncmp(line, "cd ", 3) == 0) { menu_cd(m, trim(line + 3)); return 0; }
    if (strncmp(line, "link ", 5) == 0) { menu_link(m, trim(line + 5)); return 0; }
    {
        int n = is_index(line);
        if (n) {
            FileItem *it;
            if ((size_t)n > m->items.n) { out("输入错误\n"); return 0; }
            it = &m->items.v[n - 1];
            if (it->type == 1) {
                enter(m, it->id, it->file_name);
            } else {
                char *hs = human(it->size);
                out("%s  %s\n", it->file_name, hs ? hs : "");
                free(hs);
                if (confirm("输入 1 开始下载")) download_one(m, it, 1);
            }
            return 0;
        }
    }
    out("未知命令，输入 help 查看\n");
    return 0;
}

int run_menu(Client *c, const char *outdir)
{
    Menu m;
    size_t i;
    int code = 0;
    memset(&m, 0, sizeof m);
    m.c = c;
    m.outdir = outdir;
    m.parent = str_dup("0");
    m.display = str_dup("");
    filelist_init(&m.items);
    refresh(&m);
    out("输入 help 查看命令\n> ");
    for (;;) {
        char *line = NULL, *t;
        if (!read_line(&line)) break;
        t = trim(line);
        if (g_interrupted) { free(line); code = 1; break; }
        if (*t && dispatch(&m, t)) { free(line); break; }
        out("> ");
        free(line);
    }
    for (i = 0; i < m.nstack; i++) free(m.stack[i]);
    free(m.stack);
    free(m.parent); free(m.display);
    filelist_free(&m.items);
    return code;
}
