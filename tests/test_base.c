#include "pan.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, name) do { if (!(c)) { printf("FAIL %s\n", name); fail = 1; } else { printf("PASS %s\n", name); } } while (0)

int main(void)
{
    /* 复刻真机响应形态：FileId/S3KeyFlag 混合类型、中文名、\u 转义 */
    const char *js =
        "{\"code\":0,\"message\":\"ok\",\"data\":{\"Total\":2,\"InfoList\":["
        "{\"FileName\":\"\\u4e2d\\u6587 \\u6587\\u4ef6.bin\",\"Size\":10240,\"Type\":0,"
        "\"FileId\":200,\"Etag\":\"e1\",\"S3KeyFlag\":\"0\"},"
        "{\"FileName\":\"sub\",\"Type\":1,\"Size\":0,\"FileId\":\"100\","
        "\"Etag\":\"\",\"S3KeyFlag\":0}]}}";
    JDoc d;
    Buf s;
    int data, arr, it0, it1, k, fail = 0;
    char tmp[MAX_PATH];
    const char *cfg_json = "{\"username\":\"u1\",\"password\":\"p1\",\"download_path\":\"\"}";
    FILE *f;
    Config c;

    memset(&d, 0, sizeof d);
    CHECK(json_parse(&d, js, strlen(js)) > 0, "json_parse");
    k = json_obj_get(&d, 0, "code");
    CHECK(json_int(&d, k, NULL) == 0, "code=0");
    data = json_obj_get(&d, 0, "data");
    k = json_obj_get(&d, data, "Total");
    CHECK(json_int(&d, k, NULL) == 2, "Total=2");
    arr = json_obj_get(&d, data, "InfoList");
    it0 = json_arr_at(&d, arr, 0);
    it1 = json_arr_at(&d, arr, 1);
    CHECK(it0 >= 0 && it1 >= 0, "array items");
    k = json_obj_get(&d, it0, "FileName");
    if (buf_init(&s, 64) == 0) {
        json_str(&d, k, &s);
        CHECK(strcmp(s.data, "中文 文件.bin") == 0, "unicode escape + utf8");
        buf_free(&s);
    }
    k = json_obj_get(&d, it0, "Size");
    CHECK(json_int(&d, k, NULL) == 10240, "int64 size");
    k = json_obj_get(&d, it0, "FileId");
    if (buf_init(&s, 64) == 0) {
        json_raw(&d, k, &s);
        CHECK(strcmp(s.data, "200") == 0, "raw numeric FileId");
        buf_free(&s);
    }
    k = json_obj_get(&d, it0, "S3KeyFlag");
    if (buf_init(&s, 64) == 0) {
        json_raw(&d, k, &s);
        CHECK(strcmp(s.data, "\"0\"") == 0, "raw string S3KeyFlag");
        buf_free(&s);
    }
    k = json_obj_get(&d, it1, "FileId");
    if (buf_init(&s, 64) == 0) {
        json_raw(&d, k, &s);
        CHECK(strcmp(s.data, "\"100\"") == 0, "raw string FileId");
        buf_free(&s);
    }
    json_free(&d);

    CHECK(utf8_width("中文") == 4, "utf8_width CJK");
    CHECK(utf8_width("abc") == 3, "utf8_width ascii");

    /* config.json 读取 + 环境变量覆盖 */
    snprintf(tmp, sizeof tmp, "%s\\pan_test_config.json", getenv("TEMP") ? getenv("TEMP") : ".");
    f = fopen(tmp, "wb");
    if (!f) { printf("FAIL temp config\n"); return 1; }
    fwrite(cfg_json, 1, strlen(cfg_json), f);
    fclose(f);
    CHECK(config_load(tmp, &c) == 0, "config_load");
    CHECK(c.username && strcmp(c.username, "u1") == 0, "config username");
    CHECK(c.password && strcmp(c.password, "p1") == 0, "config password");
    config_free(&c);
    _putenv("PAN123_USERNAME=envu");
    _putenv("PAN123_PASSWORD=envp");
    CHECK(config_load(tmp, &c) == 0, "config_load with env");
    CHECK(c.username && strcmp(c.username, "envu") == 0, "env override");
    config_free(&c);
    _putenv("PAN123_USERNAME=");
    _putenv("PAN123_PASSWORD=");
    remove(tmp);

    return fail ? 1 : 0;
}
