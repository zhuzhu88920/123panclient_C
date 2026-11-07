#include "pan.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

/* 26 字符映射表与 Go 版 sign.go 逐字节一致 */
static const unsigned char SIGN_TABLE[26] = {
    'a','d','e','f','g','h','l','m','y','i',
    'j','n','o','p','k','q','r','s','t','u',
    'b','c','v','w','s','z'
};

static uint32_t crc_tab[256];
static int crc_ready = 0;

static void crc_init(void)
{
    unsigned i;
    int k;
    for (i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i;
        for (k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[i] = c;
    }
    crc_ready = 1;
}

uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    if (!crc_ready) crc_init();
    for (i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

int sign_make(const char *path, int64_t ts, const char *rnd, const char *now12,
              char *key, size_t key_sz, char *val, size_t val_sz)
{
    unsigned char mapped[12];
    char tsb[32], time_sign[16], data_sign[16];
    Buf data;
    int i, n;
    if (!path || !rnd || !now12 || !key || !val || key_sz == 0 || val_sz == 0) return -1;
    if (strlen(now12) != 12) return -1;
    for (i = 0; i < 12; i++) {
        if (now12[i] < '0' || now12[i] > '9') return -1;
        mapped[i] = SIGN_TABLE[now12[i] - '0'];
    }
    snprintf(tsb, sizeof tsb, "%lld", (long long)ts);
    snprintf(time_sign, sizeof time_sign, "%u", (unsigned)crc32_ieee(mapped, 12));
    if (buf_init(&data, 128)) return -1;
    if (buf_printf(&data, "%s|%s|%s|web|3|%s", tsb, rnd, path, time_sign)) {
        buf_free(&data); return -1;
    }
    snprintf(data_sign, sizeof data_sign, "%u",
             (unsigned)crc32_ieee((const uint8_t *)data.data, data.len));
    buf_free(&data);
    if (strlen(time_sign) >= key_sz) return -1;
    memcpy(key, time_sign, strlen(time_sign) + 1);
    n = snprintf(val, val_sz, "%s-%s-%s", tsb, rnd, data_sign);
    if (n < 0 || (size_t)n >= val_sz) return -1;
    return 0;
}

int sign_live(const char *path, char *key, size_t key_sz, char *val, size_t val_sz)
{
    unsigned char b[8];
    uint64_t x = 0;
    char rnd[32], now[13];
    int i;
    if (BCryptGenRandom(NULL, b, 8, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return -1;
    for (i = 0; i < 8; i++) x = (x << 8) | b[i];
    snprintf(rnd, sizeof rnd, "%llu", (unsigned long long)(x % 10000001ULL));
    now_cst12(now);
    return sign_make(path, now_unix(), rnd, now, key, key_sz, val, val_sz);
}
