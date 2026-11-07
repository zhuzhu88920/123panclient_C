#include "pan.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_interrupted, 1);
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    Options o;
    Config cfg;
    Client c;
    char *edir = NULL, *cfgpath = NULL, *outdir = NULL;
    Buf b;
    int code = 0, rc;

    memset(&o, 0, sizeof o);
    memset(&cfg, 0, sizeof cfg);
    memset(&c, 0, sizeof c);
    out_init();
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    if (parse_args(argc, argv, &o)) {
        out("参数错误: %s\n", args_error());
        return 2;
    }
    if (o.help || (o.command && strcmp(o.command, "help") == 0)) {
        print_help();
        options_free(&o);
        return 0;
    }

    edir = exe_dir();
    if (o.config) {
        cfgpath = str_dup(o.config);
    } else {
        buf_init(&b, 256);
        buf_printf(&b, "%s\\config.json", edir);
        cfgpath = buf_take(&b);
    }
    log_init(edir);
    rc = config_load(cfgpath, &cfg);
    if (rc != 0) {
        if (rc == -2) out("config.json 解析失败\n");
        else out("缺少账号或密码：请填写 config.json，或设置 PAN123_USERNAME / PAN123_PASSWORD\n");
        out("配置文件路径: %s\n", cfgpath ? cfgpath : "");
        code = 2;
        goto done;
    }

    outdir = o.out ? str_dup(o.out) : default_download_dir(cfg.download_path);
    if (o.verbose) out("输出目录: %s\n", outdir);
    if (client_init(&c, &cfg, o.verbose)) {
        out("初始化失败: %s\n", api_error());
        code = 1;
        goto done;
    }
    if (api_login(&c)) {
        out("登录失败: %s\n", api_error());
        if (buf_init(&b, 256) == 0) {
            buf_printf(&b, "登录失败: %s", api_error());
            log_only("ERROR", b.data);
            buf_free(&b);
        }
        code = 1;
        goto done;
    }
    code = o.command ? run_cli(&c, &o, outdir) : run_menu(&c, outdir);

done:
    client_free(&c);
    config_free(&cfg);
    options_free(&o);
    free(edir); free(cfgpath); free(outdir);
    log_close();
    return code;
}
