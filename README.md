# 123panclient_C

123 网盘命令行客户端的 **C99 重构版**：功能完整复刻 Go 版，单文件 exe、免安装、仅依赖 Windows 系统 DLL。

## 为什么重构

| 项目 | Go 版 | 本版 |
|---|---|---|
| 体积 | 6.6 MB（UPX 后 2.1 MB） | 75.5 KB（77,312 B）；UPX 版 37 KB（37,888 B） |
| 运行依赖 | 静态 runtime | 仅系统自带 `KERNEL32 / WINHTTP / BCRYPT / msvcrt` |
| 第三方库 | 无 | 无 |
| 平台 | Windows | Windows 10 / 11（x64） |

TLS 由系统 Schannel 提供（实测 TLS 1.3，证书链由系统根证书校验），HTTP 由 WinHTTP 提供，JSON 与反爬签名为手写实现，不链接任何第三方库，不需要 VC++ 运行库。

## 功能

- 参数化一行命令：`list`、`download <选择器> [...]`
- 交互菜单：`ls / re / <数字> / download / cd / link / log / reload / mkdir / delete / share / upload / help / exit`
- 选择器：序号、精确文件名、忽略大小写、子串模糊、通配符、多层子目录、`--in` 前缀
- 断点续传：`.part` + `Range` + 206，中断保留残件，完成原子重命名
- 跳过已完成文件、大小校验、文件夹打包成 zip
- 进度条：控制台 `\r` 刷新，管道每 5% 一行
- 默认输出目录优先级：`--out` > `config.download_path` > 第二块固定硬盘 > 第一块 > `%USERPROFILE%\downloads` > 当前目录
- 凭据：`config.json`（与 exe 同目录）或环境变量 `PAN123_USERNAME` / `PAN123_PASSWORD`
- 控制台中文用 `WriteConsoleW`（UTF-16），管道输出 UTF-8

未实现：`upload`（分片/S3 私有协议，Go 版本身也未实现）。

## 两种可执行文件

| 文件 | 大小 | 说明 |
|---|---|---|
| `123panclient_C.exe` | 75.5 KB | 默认交付版，原生 PE，未加壳 |
| `123panclient_C_upx.exe` | 37 KB | 可选极小版，UPX `--best --lzma` 加壳 |

两者功能、依赖完全一致，任选其一即可（都是单文件、免安装）。加壳版体积减半，但个别安全软件会对 UPX 壳报启发式误报；遇到误报直接用未加壳版。`build.ps1 -Upx` 会把默认交付版原地压缩成加壳版。

## 快速开始

1. 在同目录创建 `config.json`（参考 `config.example.json`），或设置环境变量：
   ```
   set PAN123_USERNAME=你的手机号
   set PAN123_PASSWORD=你的密码
   ```
2. 运行：
   ```
   123panclient_C.exe                 交互菜单
   123panclient_C.exe list            列根目录
   123panclient_C.exe list --in OS    列子目录
   123panclient_C.exe download 7      下载根目录第 7 项
   ```

命令行选项与 Go 版一致：`--in` / `--out` / `--all` / `--dir` / `--yes` / `--config` / `--verbose` / `-h`。

## 构建

工具链：MinGW-w64 GCC（本机解压在 `D:\code\_tools\w64devkit\w64devkit\bin`，可用 `-Gcc` 指定）。

```powershell
powershell -File build.ps1          # 产出 123panclient_C.exe
powershell -File build.ps1 -Upx     # 再用 UPX --best --lzma 压缩
```

编译选项：`-std=c99 -Os -s -fno-ident -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--build-id=none`，链接 `-lwinhttp -lbcrypt`。

源码统一 **CRLF + UTF-8 BOM**（含中文），纯 ASCII 文件为 UTF-8 无 BOM。

## 测试

```powershell
powershell -File tests\run.ps1
```

覆盖：签名 4 组固定向量（与 Go 版逐字节一致）、JSON 混合类型与 Unicode、config 读取与环境变量覆盖、本地 mock 全链路（登录/分页/302 Location/href 兜底/Range 206）、续传/跳过/大小不符、选择器与参数解析、CLI list/download。

`tests/mockserver.go` 是本地协议 mock；`PAN123_API_BASE` / `PAN123_LOGIN_BASE` 两个环境变量仅用于把测试指向 mock，正式使用不设置即可。

## 协议要点

- 登录：`login.123pan.com/api/user/sign_in`（JSON body），返回长 token（实测 591 字符），不依赖 cookie。
- 签名：`<timeSign>=<时间戳>-<随机数>-<dataSign>` 单个 query 参数；两次 IEEE CRC32，UTC+8 `%Y%m%d%H%M` 经 26 字符表映射。**仅 `/b/api/file/list/new` 需要**（实测 `download_info` 免签）。
- 下载链路：`list/new` → `download_info` / `batch_download_info` → 第一跳 GET（302 `Location` 优先，body 内 `href` 兜底）→ CDN `Range` 流式下载。
- 字段类型：`FileId` 可能是数字或字符串，`S3KeyFlag` 亦然，请求体按原始 JSON 值原样回填。

## 安全与隐私

- 账号密码只从 `config.json` / 环境变量读取，不进构建产物、不打印、不写日志。
- 签名 URL 只出现在用户主动执行的 `link` 命令输出中，**永不写入日志**。
- 全程严格 TLS 证书校验（WinHTTP 默认），未设置任何忽略证书错误的选项。
- 所有变长输入（token、CDN URL、中文文件名、JSON 响应）均动态分配并做长度校验；禁用 `strcpy` / `sprintf` 等无边界写法。

## 免责

本工具用于个人管理自有 123 网盘文件，请遵守服务条款。
