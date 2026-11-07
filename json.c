#include "pan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSTACK_MAX 64

static int jd_push(JDoc *d, JType type, size_t start, size_t end, int parent)
{
    JTok *t;
    if (d->n == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 128;
        JTok *p = (JTok *)realloc(d->t, cap * sizeof(JTok));
        if (!p) return -1;
        d->t = p; d->cap = cap;
    }
    t = &d->t[d->n];
    t->type = type; t->start = start; t->end = end;
    t->child_count = 0; t->parent = parent;
    if (parent >= 0) d->t[parent].child_count++;
    return (int)d->n++;
}

void json_free(JDoc *d)
{
    if (d->t) free(d->t);
    d->t = NULL; d->n = 0; d->cap = 0; d->text = NULL; d->len = 0;
}

int json_parse(JDoc *d, const char *s, size_t len)
{
    int stack[JSTACK_MAX];
    int top = 0;
    size_t i = 0;
    memset(d, 0, sizeof *d);
    d->text = s; d->len = len;
    stack[0] = -1;
    while (i < len) {
        char c = s[i];
        int parent = stack[top];
        int idx;
        if (is_space_ch(c)) { i++; continue; }
        if (c == '{' || c == '[') {
            if (top + 1 >= JSTACK_MAX) goto bad;
            idx = jd_push(d, c == '{' ? J_OBJ : J_ARR, i, i, parent);
            if (idx < 0) goto bad;
            stack[++top] = idx;
            i++;
            continue;
        }
        if (c == '}' || c == ']') {
            if (top <= 0) goto bad;
            d->t[stack[top]].end = i;
            top--;
            i++;
            continue;
        }
        if (c == ',' || c == ':') { i++; continue; }
        if (c == '"') {
            size_t st = i + 1;
            i++;
            while (i < len) {
                if (s[i] == '\\') { i += 2; continue; }
                if (s[i] == '"') break;
                i++;
            }
            if (i >= len) goto bad;
            if (jd_push(d, J_STR, st, i, parent) < 0) goto bad;
            i++;
            continue;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t st = i;
            while (i < len && (s[i] == '-' || s[i] == '+' || s[i] == '.' ||
                   s[i] == 'e' || s[i] == 'E' || (s[i] >= '0' && s[i] <= '9'))) i++;
            if (jd_push(d, J_NUM, st, i, parent) < 0) goto bad;
            continue;
        }
        if (len - i >= 4 && memcmp(s + i, "true", 4) == 0) {
            if (jd_push(d, J_BOOL, i, i + 4, parent) < 0) goto bad;
            i += 4; continue;
        }
        if (len - i >= 5 && memcmp(s + i, "false", 5) == 0) {
            if (jd_push(d, J_BOOL, i, i + 5, parent) < 0) goto bad;
            i += 5; continue;
        }
        if (len - i >= 4 && memcmp(s + i, "null", 4) == 0) {
            if (jd_push(d, J_NULL, i, i + 4, parent) < 0) goto bad;
            i += 4; continue;
        }
        goto bad;
    }
    if (top != 0 || d->n == 0) goto bad;
    return (int)d->n;
bad:
    json_free(d);
    return -1;
}

int json_obj_get(const JDoc *d, int obj, const char *key)
{
    int i;
    if (obj < 0 || obj >= (int)d->n || d->t[obj].type != J_OBJ || !key) return -1;
    i = obj + 1;
    while (i < (int)d->n) {
        int k, v;
        while (i < (int)d->n && d->t[i].parent != obj) i++;
        if (i >= (int)d->n) break;
        k = i++;
        while (i < (int)d->n && d->t[i].parent != obj) i++;
        if (i >= (int)d->n) break;
        v = i++;
        if (d->t[k].type == J_STR) {
            size_t kl = d->t[k].end - d->t[k].start;
            if (kl == strlen(key) && memcmp(d->text + d->t[k].start, key, kl) == 0)
                return v;
        }
    }
    return -1;
}

int json_arr_at(const JDoc *d, int arr, size_t idx)
{
    size_t k = 0;
    int i;
    if (arr < 0 || arr >= (int)d->n || d->t[arr].type != J_ARR) return -1;
    for (i = arr + 1; i < (int)d->n; i++) {
        if (d->t[i].parent == arr) {
            if (k == idx) return i;
            k++;
        }
    }
    return -1;
}

static int hex4(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

int json_str(const JDoc *d, int tok, Buf *out)
{
    size_t i, end;
    if (!d || !out || tok < 0 || tok >= (int)d->n || d->t[tok].type != J_STR) return -1;
    i = d->t[tok].start;
    end = d->t[tok].end;
    while (i < end) {
        char c = d->text[i];
        if (c == '\\' && i + 1 < end) {
            char e = d->text[i + 1];
            i += 2;
            switch (e) {
            case 'n': if (buf_append(out, "\n", 1)) return -1; break;
            case 'r': if (buf_append(out, "\r", 1)) return -1; break;
            case 't': if (buf_append(out, "\t", 1)) return -1; break;
            case 'b': if (buf_append(out, "\b", 1)) return -1; break;
            case 'f': if (buf_append(out, "\f", 1)) return -1; break;
            case 'u': {
                uint32_t cp;
                if (i + 4 > end || hex4(d->text + i, &cp)) return -1;
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= end &&
                    d->text[i] == '\\' && d->text[i + 1] == 'u') {
                    uint32_t lo;
                    if (hex4(d->text + i + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        i += 6;
                    }
                }
                if (utf8_append_cp(out, cp)) return -1;
                break;
            }
            default:
                if (buf_append(out, &e, 1)) return -1;
            }
        } else {
            if (buf_append(out, &c, 1)) return -1;
            i++;
        }
    }
    return 0;
}

/* 返回可直接嵌入请求体的原始 JSON 值文本：字符串重新包引号并保留原转义 */
int json_raw(const JDoc *d, int tok, Buf *out)
{
    const JTok *t;
    if (!d || !out || tok < 0 || tok >= (int)d->n) return -1;
    t = &d->t[tok];
    if (t->type == J_STR) {
        if (buf_append(out, "\"", 1)) return -1;
        if (buf_append(out, d->text + t->start, t->end - t->start)) return -1;
        return buf_append(out, "\"", 1);
    }
    return buf_append(out, d->text + t->start, t->end - t->start);
}

int64_t json_int(const JDoc *d, int tok, int *ok)
{
    char tmp[64];
    size_t n;
    if (ok) *ok = 0;
    if (!d || tok < 0 || tok >= (int)d->n || d->t[tok].type != J_NUM) return 0;
    n = d->t[tok].end - d->t[tok].start;
    if (n == 0 || n >= sizeof tmp) return 0;
    memcpy(tmp, d->text + d->t[tok].start, n);
    tmp[n] = 0;
    if (ok) *ok = 1;
    return (int64_t)strtoll(tmp, NULL, 10);
}

int json_bool(const JDoc *d, int tok, int *ok)
{
    size_t n;
    if (ok) *ok = 0;
    if (!d || tok < 0 || tok >= (int)d->n || d->t[tok].type != J_BOOL) return 0;
    n = d->t[tok].end - d->t[tok].start;
    if (ok) *ok = 1;
    return (n == 4 && memcmp(d->text + d->t[tok].start, "true", 4) == 0) ? 1 : 0;
}
