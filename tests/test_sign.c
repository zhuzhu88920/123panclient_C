#include "pan.h"
#include <stdio.h>
#include <string.h>

/* 与 Go 版 sign_test.go 完全相同的 4 组固定时钟/随机数向量 */
static const struct {
    const char *path;
    long long ts;
    const char *rnd;
    const char *now12;
    const char *key;
    const char *val;
} V[] = {
    { "/b/api/file/list/new", 1793966700LL, "1234567", "202611062005",
      "431471781", "1793966700-1234567-1310521917" },
    { "/b/api/file/list/new", 1704038400LL, "0", "202401010000",
      "365709358", "1704038400-0-312142898" },
    { "/a/api/file/download_info", 1793970000LL, "9999999", "202611062100",
      "1721812714", "1793970000-9999999-716450645" },
    { "/a/api/file/download_info", 1749962040LL, "42", "202506151234",
      "4199734838", "1749962040-42-1604813972" }
};

int main(void)
{
    size_t i;
    int fail = 0;
    for (i = 0; i < sizeof V / sizeof V[0]; i++) {
        char k[32], v[128];
        if (sign_make(V[i].path, (int64_t)V[i].ts, V[i].rnd, V[i].now12,
                      k, sizeof k, v, sizeof v) != 0) {
            printf("FAIL #%zu sign_make error\n", i + 1);
            fail = 1;
            continue;
        }
        if (strcmp(k, V[i].key) != 0 || strcmp(v, V[i].val) != 0) {
            printf("FAIL #%zu\n  got  key=%s val=%s\n  want key=%s val=%s\n",
                   i + 1, k, v, V[i].key, V[i].val);
            fail = 1;
        } else {
            printf("PASS #%zu\n", i + 1);
        }
    }
    return fail ? 1 : 0;
}
