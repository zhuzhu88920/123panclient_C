#include "pan.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define LIST_PATH  "/b/api/file/list/new"
#define DL_PATH    "/a/api/file/download_info"
#define BATCH_PATH "/a/api/file/batch_download_info"
#define TRASH_PATH "/a/api/file/trash"
#define SHARE_PATH "/a/api/share/create"
#define MKDIR_PATH "/a/api/file/upload_request"

static char g_api_err[512];

const char *api_error(void) { return g_api_err[0] ? g_api_err : "接口调用失败"; }

static void api_seterr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_api_err, sizeof g_api_err, fmt, ap);
    va_end(ap);
}

/* 测试用的 base 覆盖；未设置时指向官方端点 */
static const char *api_base(void)
{
    const char *e = getenv("PAN123_API_BASE");
    return (e && *e) ? e : "https://yun.123pan.com";
}
static const char *login_base(void)
{
    const char *e = getenv("PAN123_LOGIN_BASE");
    return (e && *e) ? e : "https://login.123pan.com";
}

static int url_encode(Buf *b, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    for (; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~') {
            if (buf_append(b, p, 1)) return -1;
        } else {
            char t[3];
            t[0] = '%'; t[1] = hex[*p >> 4]; t[2] = hex[*p & 15];
            if (buf_append(b, t, 3)) return -1;
        }
    }
    return 0;
}

void filelist_init(FileList *l) { memset(l, 0, sizeof *l); }

void fileitem_free(FileItem *it)
{
    if (!it) return;
    free(it->file_name); free(it->file_id); free(it->id);
    free(it->etag); free(it->s3key_flag);
    memset(it, 0, sizeof *it);
}

void filelist_free(FileList *l)
{
    size_t i;
    if (!l) return;
    for (i = 0; i < l->n; i++) fileitem_free(&l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

int filelist_push(FileList *l, FileItem it)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 128;
        FileItem *p = (FileItem *)realloc(l->v, cap * sizeof(FileItem));
        if (!p) { fileitem_free(&it); return -1; }
        l->v = p; l->cap = cap;
    }
    l->v[l->n++] = it;
    return 0;
}

int client_init(Client *c, const Config *cfg, int verbose)
{
    unsigned char b[16];
    int i;
    memset(c, 0, sizeof *c);
    c->cfg = cfg;
    c->verbose = verbose;
    if (BCryptGenRandom(NULL, b, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        api_seterr("系统随机数不可用");
        return -1;
    }
    for (i = 0; i < 16; i++) snprintf(c->login_uuid + i * 2, 3, "%02x", b[i]);
    return 0;
}

void client_free(Client *c)
{
    if (!c) return;
    free(c->token);
    c->token = NULL;
    c->cfg = NULL;
}

int api_raw(Client *c, const char *method, const char *url,
            const char *body, HttpResponse *r)
{
    const char *names[12], *vals[12];
    size_t nh = 0;
    Buf auth;
    int rc;
    buf_init(&auth, 64);
    names[nh] = "User-Agent";        vals[nh++] = APP_VERSION;
    names[nh] = "Content-Type";      vals[nh++] = "application/json";
    names[nh] = "osversion";         vals[nh++] = "Android_7.1.2";
    names[nh] = "loginuuid";         vals[nh++] = c->login_uuid;
    names[nh] = "platform";          vals[nh++] = "android";
    names[nh] = "devicetype";        vals[nh++] = "M2101K9C";
    names[nh] = "x-channel";         vals[nh++] = "1004";
    names[nh] = "devicename";        vals[nh++] = "Xiaomi";
    names[nh] = "app-version";       vals[nh++] = "61";
    names[nh] = "x-app-version";     vals[nh++] = "2.4.0";
    if (c->token) {
        if (buf_printf(&auth, "Bearer %s", c->token)) { buf_free(&auth); return -1; }
        names[nh] = "authorization"; vals[nh++] = auth.data;
    }
    rc = http_simple(method, url, names, vals, nh, body,
                     body ? strlen(body) : 0, 1, r);
    buf_free(&auth);
    if (rc != 0) api_seterr("%s", http_error());
    return rc;
}

/* 解析统一 envelope，成功返回 data token 下标；调用者负责 json_free */
static int envelope(const HttpResponse *r, JDoc *d, int *data_out)
{
    int root, k, data;
    long long code;
    char *msg = NULL;
    Buf s;
    memset(d, 0, sizeof *d);
    if (r->status < 200 || r->status >= 300) {
        api_seterr("HTTP %d", r->status);
        return -1;
    }
    if (json_parse(d, r->body.data, r->body.len) < 0 || d->n == 0) {
        api_seterr("HTTP %d 返回非 JSON", r->status);
        json_free(d); return -1;
    }
    root = 0;
    k = json_obj_get(d, root, "code");
    code = json_int(d, k, NULL);
    k = json_obj_get(d, root, "message");
    if (k >= 0 && buf_init(&s, 64) == 0) { json_str(d, k, &s); msg = buf_take(&s); }
    data = json_obj_get(d, root, "data");
    if (code != 0) {
        api_seterr("%s (code=%lld)", msg ? msg : "", code);
        free(msg); json_free(d); return -1;
    }
    free(msg);
    *data_out = data;
    return 0;
}

int api_login(Client *c)
{
    Buf body = {0}, u = {0};
    HttpResponse r = {0};
    JDoc d = {0};
    int k, data, ret = -1;
    long long code;
    char *token = NULL;

    buf_init(&body, 256); buf_init(&u, 256);
    buf_append_cstr(&body, "{\"type\":1,\"passport\":\"");
    json_escape(&body, c->cfg->username);
    buf_append_cstr(&body, "\",\"password\":\"");
    json_escape(&body, c->cfg->password);
    buf_append_cstr(&body, "\"}");
    buf_printf(&u, "%s/api/user/sign_in", login_base());
    if (api_raw(c, "POST", u.data, body.data, &r)) goto done;
    memset(&d, 0, sizeof d);
    if (json_parse(&d, r.body.data, r.body.len) < 0 || d.n == 0) {
        api_seterr("登录响应不是 JSON"); goto done;
    }
    k = json_obj_get(&d, 0, "code");
    code = json_int(&d, k, NULL);
    data = json_obj_get(&d, 0, "data");
    k = json_obj_get(&d, data, "token");
    if (k >= 0) { Buf s; if (buf_init(&s, 64) == 0) { json_str(&d, k, &s); token = buf_take(&s); } }
    if (code != 200 || !token || !*token) {
        Buf s;
        char *msg = NULL;
        k = json_obj_get(&d, 0, "message");
        if (k >= 0 && buf_init(&s, 64) == 0) { json_str(&d, k, &s); msg = buf_take(&s); }
        api_seterr("登录失败: %s (code=%lld)", msg ? msg : "", code);
        free(msg); goto done;
    }
    free(c->token);
    c->token = token;
    token = NULL;
    ret = 0;
done:
    free(token);
    buf_free(&body); buf_free(&u);
    http_response_free(&r);
    json_free(&d);
    return ret;
}

static int parse_item(const JDoc *d, int tok, FileItem *it)
{
    Buf s;
    int k;
    memset(it, 0, sizeof *it);
    k = json_obj_get(d, tok, "FileName");
    if (k >= 0 && buf_init(&s, 64) == 0) { json_str(d, k, &s); it->file_name = buf_take(&s); }
    k = json_obj_get(d, tok, "Size");   it->size = json_int(d, k, NULL);
    k = json_obj_get(d, tok, "Type");   it->type = (int)json_int(d, k, NULL);
    k = json_obj_get(d, tok, "FileId");
    if (k >= 0) {
        if (buf_init(&s, 64) == 0) { json_raw(d, k, &s); it->file_id = buf_take(&s); }
        if (d->t[k].type == J_STR) {
            if (buf_init(&s, 64) == 0) { json_str(d, k, &s); it->id = buf_take(&s); }
        } else {
            it->id = str_dup(it->file_id ? it->file_id : "");
        }
    }
    k = json_obj_get(d, tok, "Etag");
    if (k >= 0 && buf_init(&s, 64) == 0) { json_str(d, k, &s); it->etag = buf_take(&s); }
    k = json_obj_get(d, tok, "S3KeyFlag");
    if (k >= 0 && buf_init(&s, 64) == 0) { json_raw(d, k, &s); it->s3key_flag = buf_take(&s); }
    if (!it->file_name || !it->id || !it->file_id) { fileitem_free(it); return -1; }
    return 0;
}

int api_list(Client *c, const char *parent, FileList *out)
{
    int page;
    for (page = 1; page <= 100; page++) {
        char key[32], val[128];
        Buf u;
        HttpResponse r = {0};
        JDoc d = {0};
        int data, arr, k, i, ret = -1, stop = 1;
        long long total = 0;
        size_t before;
        memset(&d, 0, sizeof d);
        buf_init(&u, 512);
        if (sign_live(LIST_PATH, key, sizeof key, val, sizeof val)) {
            api_seterr("签名生成失败"); buf_free(&u); return -1;
        }
        buf_printf(&u, "%s%s?%s=%s&driveId=0&limit=100&next=0&orderBy=file_id&orderDirection=desc&parentFileId=",
                   api_base(), LIST_PATH, key, val);
        url_encode(&u, parent);
        buf_printf(&u, "&trashed=False&SearchData=&Page=%d&OnlyLookAbnormalFile=0", page);
        if (api_raw(c, "GET", u.data, NULL, &r)) { buf_free(&u); goto done; }
        if (envelope(&r, &d, &data)) goto done;
        k = json_obj_get(&d, data, "Total");
        total = json_int(&d, k, NULL);
        arr = json_obj_get(&d, data, "InfoList");
        before = out->n;
        for (i = 0; ; i++) {
            int t = json_arr_at(&d, arr, (size_t)i);
            FileItem it;
            if (t < 0) break;
            if (parse_item(&d, t, &it) == 0 && filelist_push(out, it) != 0) goto done;
        }
        ret = 0;
        if (out->n == before) { goto done; }
        if (total >= 0 && (long long)out->n >= total) { goto done; }
        stop = 0;   /* 还有下一页 */
done:
        buf_free(&u);
        http_response_free(&r);
        json_free(&d);
        if (ret != 0) return -1;
        if (stop) return 0;
    }
    return 0;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int valid_abs_url(const char *s)
{
    size_t n;
    if (!s) return 0;
    n = strlen(s);
    if (n >= 11 && strncmp(s, "https://", 8) == 0) return 1;
    if (n >= 10 && strncmp(s, "http://", 7) == 0) return 1;
    return 0;
}

/* 结构化扫描 href 属性（不做正则），只接受绝对 http(s) URL */
static char *extract_href(const char *body, size_t len)
{
    size_t pos = 0;
    if (!body) return NULL;
    while (pos + 4 <= len) {
        size_t i = pos, j, start, end;
        char *cand;
        int found = 0;
        for (; i + 4 <= len; i++) {
            if (lc(body[i]) == 'h' && lc(body[i+1]) == 'r' &&
                lc(body[i+2]) == 'e' && lc(body[i+3]) == 'f') { found = 1; break; }
        }
        if (!found) break;
        j = i + 4;
        while (j < len && is_space_ch(body[j])) j++;
        if (j >= len || body[j] != '=') { pos = i + 4; continue; }
        j++;
        while (j < len && is_space_ch(body[j])) j++;
        if (j < len && (body[j] == '\'' || body[j] == '"')) {
            char q = body[j++];
            start = j;                 /* 跳过开引号，候选值不含引号 */
            end = j;
            while (end < len && body[end] != q) end++;
        } else {
            start = j;
            end = j;
            while (end < len && !is_space_ch(body[end]) && body[end] != '>' &&
                   body[end] != '\'' && body[end] != '"') end++;
        }
        while (start < end && is_space_ch(body[start])) start++;
        while (end > start && is_space_ch(body[end - 1])) end--;
        cand = str_dup_n(body + start, end - start);
        if (cand && valid_abs_url(cand)) return cand;
        free(cand);
        pos = end + 1;
        if (pos >= len) break;
    }
    return NULL;
}

int api_download_url(Client *c, const FileItem *it, char **url)
{
    Buf body = {0}, u = {0}, s = {0};
    HttpResponse r = {0}, r2 = {0};
    JDoc d = {0};
    const char *endpoint, *names[1], *vals[1];
    char *first = NULL, *final = NULL;
    int data, k, ret = -1;

    *url = NULL;
    buf_init(&body, 512); buf_init(&u, 256); buf_init(&s, 256);
    if (it->type == 1) {
        endpoint = BATCH_PATH;
        buf_printf(&body, "{\"fileIdList\":[{\"fileId\":%s}]}",
                   it->file_id ? it->file_id : "0");
    } else {
        endpoint = DL_PATH;
        buf_append_cstr(&body, "{\"driveId\":0,\"etag\":\"");
        json_escape(&body, it->etag ? it->etag : "");
        buf_append_cstr(&body, "\",\"fileId\":");
        buf_append_cstr(&body, it->file_id ? it->file_id : "0");
        buf_append_cstr(&body, ",\"s3keyFlag\":");
        buf_append_cstr(&body, it->s3key_flag ? it->s3key_flag : "0");
        buf_printf(&body, ",\"type\":%d,\"fileName\":\"", it->type);
        json_escape(&body, it->file_name ? it->file_name : "");
        buf_printf(&body, "\",\"size\":%lld}", (long long)it->size);
    }
    buf_printf(&u, "%s%s", api_base(), endpoint);
    memset(&d, 0, sizeof d);
    if (api_raw(c, "POST", u.data, body.data, &r)) goto done;
    if (envelope(&r, &d, &data)) goto done;
    k = json_obj_get(&d, data, "DownloadUrl");
    if (k >= 0 && buf_init(&s, 256) == 0) { json_str(&d, k, &s); first = buf_take(&s); }
    if (!first || !*first) { api_seterr("未返回下载链接"); goto done; }

    names[0] = "User-Agent"; vals[0] = APP_VERSION;
    if (http_simple("GET", first, names, vals, 1, NULL, 0, 0, &r2)) {
        api_seterr("%s", http_error()); goto done;
    }
    if (r2.location && valid_abs_url(r2.location)) final = str_dup(r2.location);
    else final = extract_href(r2.body.data, r2.body.len);
    if (!final) api_seterr("无法解析跳转链接 (HTTP %d)", r2.status);
    else { *url = final; final = NULL; ret = 0; }
    http_response_free(&r2);
done:
    free(first); free(final);
    buf_free(&body); buf_free(&u); buf_free(&s);
    http_response_free(&r);
    json_free(&d);
    return ret;
}

int api_mkdir(Client *c, const char *parent, const char *name)
{
    Buf body = {0}, u = {0};
    HttpResponse r = {0};
    JDoc d = {0};
    int data, ret = -1;
    buf_init(&body, 512); buf_init(&u, 256);
    buf_append_cstr(&body, "{\"driveId\":0,\"etag\":\"\",\"fileName\":\"");
    json_escape(&body, name);
    buf_append_cstr(&body, "\",\"parentFileId\":\"");
    json_escape(&body, parent);
    buf_append_cstr(&body, "\",\"size\":0,\"type\":1,\"duplicate\":1,\"NotReuse\":true,\"event\":\"newCreateFolder\",\"operateType\":1}");
    buf_printf(&u, "%s%s", api_base(), MKDIR_PATH);
    memset(&d, 0, sizeof d);
    if (api_raw(c, "POST", u.data, body.data, &r)) goto done;
    if (envelope(&r, &d, &data)) goto done;
    ret = 0;
done:
    buf_free(&body); buf_free(&u);
    http_response_free(&r); json_free(&d);
    return ret;
}

int api_trash(Client *c, const FileItem *it)
{
    Buf body = {0}, u = {0};
    HttpResponse r = {0};
    JDoc d = {0};
    int data, ret = -1;
    buf_init(&body, 512); buf_init(&u, 256);
    buf_append_cstr(&body, "{\"driveId\":0,\"fileTrashInfoList\":{\"FileName\":\"");
    json_escape(&body, it->file_name ? it->file_name : "");
    buf_printf(&body, "\",\"Size\":%lld,\"Type\":%d,\"FileId\":%s,\"Etag\":\"",
               (long long)it->size, it->type, it->file_id ? it->file_id : "0");
    json_escape(&body, it->etag ? it->etag : "");
    buf_append_cstr(&body, "\",\"S3KeyFlag\":");
    buf_append_cstr(&body, it->s3key_flag ? it->s3key_flag : "0");
    buf_append_cstr(&body, "},\"operation\":true}");
    buf_printf(&u, "%s%s", api_base(), TRASH_PATH);
    memset(&d, 0, sizeof d);
    if (api_raw(c, "POST", u.data, body.data, &r)) goto done;
    if (envelope(&r, &d, &data)) goto done;
    ret = 0;
done:
    buf_free(&body); buf_free(&u);
    http_response_free(&r); json_free(&d);
    return ret;
}

int api_share(Client *c, const char *ids_csv, char **share_key)
{
    Buf body = {0}, u = {0}, s = {0};
    HttpResponse r = {0};
    JDoc d = {0};
    int data, k, ret = -1;
    *share_key = NULL;
    buf_init(&body, 512); buf_init(&u, 256); buf_init(&s, 64);
    buf_append_cstr(&body, "{\"driveId\":0,\"expiration\":\"2099-12-12T08:00:00+08:00\",\"fileIdList\":\"");
    json_escape(&body, ids_csv);
    buf_append_cstr(&body, "\",\"shareName\":\"My Share\",\"sharePwd\":\"\",\"event\":\"shareCreate\"}");
    buf_printf(&u, "%s%s", api_base(), SHARE_PATH);
    memset(&d, 0, sizeof d);
    if (api_raw(c, "POST", u.data, body.data, &r)) goto done;
    if (envelope(&r, &d, &data)) goto done;
    k = json_obj_get(&d, data, "ShareKey");
    if (k >= 0 && buf_init(&s, 64) == 0) { json_str(&d, k, &s); *share_key = buf_take(&s); }
    if (!*share_key || !**share_key) { api_seterr("分享失败：未返回 ShareKey"); goto done; }
    ret = 0;
done:
    buf_free(&body); buf_free(&u); buf_free(&s);
    http_response_free(&r); json_free(&d);
    return ret;
}
