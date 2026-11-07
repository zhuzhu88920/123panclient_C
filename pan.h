#ifndef PAN123_H
#define PAN123_H

#include <stdint.h>
#include <stddef.h>

/* 公共类型与接口。所有变长字符串均为 UTF-8，均带长度校验，调用方负责释放。 */

typedef struct { char *data; size_t len, cap; } Buf;

int  buf_init(Buf *b, size_t cap);
void buf_free(Buf *b);
int  buf_reserve(Buf *b, size_t need);
int  buf_append(Buf *b, const void *p, size_t n);
int  buf_append_cstr(Buf *b, const char *s);
int  buf_printf(Buf *b, const char *fmt, ...);
char *buf_take(Buf *b);
char *str_dup(const char *s);
char *str_dup_n(const char *s, size_t n);
void *xmalloc(size_t n);

int  utf8_to_utf16(const char *s, wchar_t **out);
int  utf16_to_utf8(const wchar_t *s, char **out);
int  utf8_next(const char *s, size_t len, size_t *i, uint32_t *cp);
int  utf8_append_cp(Buf *b, uint32_t cp);
int  utf8_width(const char *s);
int  json_escape(Buf *b, const char *s);
int  is_space_ch(char c);

int64_t  now_unix(void);
void     now_cst12(char out[13]);
uint64_t tick_ms(void);

/* 控制台输出 / 输入 */
void out_init(void);
void out(const char *fmt, ...);
int  stdout_is_console(void);
int  read_line(char **line);

/* 日志 */
int  log_init(const char *dir);
void log_close(void);
void log_only(const char *level, const char *msg);

/* 配置 */
typedef struct { char *username; char *password; char *download_path; } Config;
int  config_load(const char *path, Config *c);
void config_free(Config *c);
char *exe_dir(void);
char *default_download_dir(const char *configured);

/* JSON（手写 tokenizer，保留原始 token 文本） */
typedef enum { J_OBJ = 1, J_ARR, J_STR, J_NUM, J_BOOL, J_NULL } JType;
typedef struct {
    JType  type;
    size_t start, end;      /* J_STR: 引号内；其他: 原始文本 */
    size_t child_count;     /* 容器直接子节点数 */
    int    parent;          /* 父 token 下标，根为 -1 */
} JTok;
typedef struct {
    const char *text;
    size_t len;
    JTok *t;
    size_t n, cap;
} JDoc;

int    json_parse(JDoc *d, const char *text, size_t len);
void   json_free(JDoc *d);
int    json_obj_get(const JDoc *d, int obj, const char *key);
int    json_arr_at(const JDoc *d, int arr, size_t idx);
int    json_str(const JDoc *d, int tok, Buf *out);        /* 解码后的 UTF-8 */
int    json_raw(const JDoc *d, int tok, Buf *out);        /* 合法的原始 JSON 值文本 */
int64_t json_int(const JDoc *d, int tok, int *ok);
int    json_bool(const JDoc *d, int tok, int *ok);

/* 反爬签名 */
uint32_t crc32_ieee(const uint8_t *data, size_t len);
int sign_make(const char *path, int64_t ts, const char *rnd, const char *now12,
              char *key, size_t key_sz, char *val, size_t val_sz);
int sign_live(const char *path, char *key, size_t key_sz, char *val, size_t val_sz);

/* HTTP */
typedef struct {
    int     status;
    char   *location;
    int64_t content_length;
    Buf     body;
} HttpResponse;
void http_response_init(HttpResponse *r);
void http_response_free(HttpResponse *r);
int  http_simple(const char *method, const char *url,
                 const char **names, const char **vals, size_t nh,
                 const char *body, size_t body_len, int follow_redirect,
                 HttpResponse *r);
typedef struct HttpStream HttpStream;
const char *http_error(void);
int  http_stream_open(const char *url, const char *range, HttpStream **s,
                      int *status, int64_t *content_length);
int  http_stream_read(HttpStream *s, void *buf, size_t cap, size_t *got);
void http_stream_close(HttpStream *s);

/* API */
typedef struct {
    char   *file_name;
    int64_t size;
    int     type;
    char   *file_id;      /* 原始 JSON 值文本，保类型 */
    char   *id;           /* 去引号后的字符串 */
    char   *etag;
    char   *s3key_flag;   /* 原始 JSON 值文本，保类型 */
} FileItem;
typedef struct { FileItem *v; size_t n, cap; } FileList;
void filelist_init(FileList *l);
void filelist_free(FileList *l);
int  filelist_push(FileList *l, FileItem it);
void fileitem_free(FileItem *it);

typedef struct { char *token; int verbose; char login_uuid[33]; const Config *cfg; } Client;
const char *api_error(void);
int  client_init(Client *c, const Config *cfg, int verbose);
void client_free(Client *c);
int  api_login(Client *c);
int  api_list(Client *c, const char *parent, FileList *out);
int  api_download_url(Client *c, const FileItem *it, char **url);
int  api_mkdir(Client *c, const char *parent, const char *name);
int  api_trash(Client *c, const FileItem *it);
int  api_share(Client *c, const char *ids_csv, char **share_key);
int  api_raw(Client *c, const char *method, const char *url, const char *body, HttpResponse *r);

/* 下载 */
typedef struct { int skipped; char *path; int64_t bytes; } DownloadResult;
const char *download_error(void);
void download_result_free(DownloadResult *r);
int  download_file(Client *c, const FileItem *it, const char *outdir, DownloadResult *r);
char *safe_name(const char *s);
char *human(int64_t n);

/* 进度 */
typedef struct Progress Progress;
Progress *progress_new(void);
void progress_update(Progress *p, int64_t done, int64_t total);
void progress_clear(Progress *p);
void progress_free(Progress *p);

/* 选择器 */
typedef struct { Client *c; } Resolver;
const char *resolve_error(void);
char **split_selector(const char *s, size_t *n);
void   split_selector_free(char **v, size_t n);
int    is_index(const char *s);
int    has_glob(const char *s);
char  *join_path(const char *base, const char *name);
const char *dir_label(const char *s);
int  resolve_dir(Resolver *r, char **segs, size_t n, char **parent_out,
                 char **display_out, FileList *items);
int  resolve_select(Resolver *r, const char *selector, const char *in_dir,
                    int allow_dir, int match_all, FileList *hits, char **display_out);
int  glob_match(const char *pat, const char *name);

/* CLI / 菜单 */
typedef struct {
    char *command;
    char **args; size_t nargs;
    char *in, *out, *config;
    int all, dir, yes, verbose, help;
} Options;
const char *args_error(void);
int  parse_args(int argc, char **argv, Options *o);
void options_free(Options *o);
void print_help(void);
void print_list(const char *display, const FileList *l);
int  run_cli(Client *c, Options *o, const char *outdir);
int  run_menu(Client *c, const char *outdir);

extern const char *APP_VERSION;
extern volatile long g_interrupted;

#endif /* PAN123_H */
