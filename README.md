# Codex Timezone Sync（Codex 时区同步）

用于 [CLIProxyAPI（CPA）](https://github.com/router-for-me/CLIProxyAPI) 的 Codex 请求时区同步插件。适合 OAuth 账号通过代理出口访问 Codex 时，需要让请求内的日期、时区与代理出口所在地区保持一致的场景。

**当前版本：v0.5.1。目标平台：Linux amd64 / arm64、macOS amd64 / arm64、Windows amd64。** 采用 CPA C ABI v1，带中文管理面板。各平台发布包只有在 GitHub Actions 对应系统原生构建与测试成功后才发布。

## 功能

- 根据代理出口 IP 检测 IANA 时区及当地日期；对 Codex 请求中的环境日期和时区做校正，而不修改宿主机系统时间。
- 提供中文管理面板，查看当前出口 IP、时区、最近同步记录及模型一致性观测。
- 支持启用/禁用时区改写、活动日志、调试日志。
- 支持手动点击「获取」重新检测出口时区（需要部署独立辅助服务）。
- 统计成功和失败的改写次数，可清空统计；最近活动记录显示协议并支持清空。
- 仅在请求改写失败时保存原始 Codex 请求体，可在管理界面查看或清空。
- 时区状态不可用或过期时采用 *fail-open* 策略，不主动阻断 Codex 原始请求。

> 注意：插件只校正请求内容，不改变浏览器指纹、OAuth 登录地点、HTTP 头或主机系统时区；也不保证代理 IP 的区域、登录设备指纹完全一致。

## 从 CPA 插件商店安装

在 CPA v7.2.46 或兼容新版中打开「插件商店」，搜索 **Codex 时区同步** 或 **codex-timezone-sync**。若官方收录尚未合并，可先添加本仓库提供的第三方商店地址：

```text
https://raw.githubusercontent.com/LonelyHerbivore/codex-timezone-sync/main/registry.json
```

或者在 CPA 配置中指定：

```yaml
plugins:
  enabled: true
  store-sources:
    - "https://raw.githubusercontent.com/LonelyHerbivore/codex-timezone-sync/main/registry.json"
  configs:
    codex-timezone-sync:
      enabled: true
```

从插件商店安装后确保单插件开关打开，并检查日志中插件已注册。Release 提供 `linux_amd64`、`linux_arm64`、`darwin_amd64`、`darwin_arm64`、`windows_amd64` 五种动态库 ZIP（分别含 `.so`、`.dylib`、`.dll`）和 `checksums.txt`。插件商店只安装动态库，**不会自动安装主机端出口时区检测辅助服务**，参阅下一节。

## 出口时区检测与手动刷新

插件需要一个定期更新的状态文件。默认情况下容器内读取：

```text
/CLIProxyAPI/data/codex-timezone-sync.state
```

状态格式（由附带的 Python 辅助脚本生成）：

```text
America/Los_Angeles|2026-10-12|PDT|-07:00
66.92.223.4
```

辅助服务所在主机需有 Python 3.9+（含 `zoneinfo` / Windows 的 `tzdata` 包）、可访问代理出口。除预置的 Linux Docker 探测模式外，macOS / Windows 建议显式设置 `CODEX_TZ_PROXY`，以确保与 CPA 上游出口一致。推荐通过环境变量显式配置路径和代理：

```bash
export CODEX_TZ_STATE=/你的共享数据目录/codex-timezone-sync.state
export CODEX_TZ_REFRESH_SOCKET=/你的共享数据目录/codex-timezone-sync-refresh.sock
export CODEX_TZ_PROXY=http://127.0.0.1:7890

# 单次检测（可通过 systemd timer / cron 定时执行）
python3 refresh_egress.py

# 常驻服务：响应中文管理面板中的「获取」
python3 refresh_egress.py --serve
```

其中 `CODEX_TZ_PROXY` 必须指向 **Codex 实际使用的同一个出口代理**；请勿让检测脚本走不同 IP。未设置时脚本会尝试按开发环境中的 `mihomo` 容器和 `cpa-proxy-net` 网络定位代理，该默认值并不适用于所有部署。宿主机的状态文件与 Unix Socket 需映射到 CPA 容器的 `/CLIProxyAPI/data/` 下；容器中 Socket 默认路径是 `/CLIProxyAPI/data/codex-timezone-sync-refresh.sock`。如需自定义，设置容器环境变量 `CODEX_TZ_STATE_FILE` / `CODEX_TZ_REFRESH_SOCKET`。

手动刷新依赖 Unix Socket 辅助服务并不是安装插件的必要条件；如果使用计划任务定期写入状态文件，自动改写仍然可用。建议状态文件刷新间隔不超过 30 分钟，插件对超过约 2 小时的状态会视为过期。

### macOS / Windows 补充说明

macOS 与 Linux 一样使用 Unix Domain Socket 进行手动「获取」。Windows 系统的 Python 不支持 Unix Socket，因此 v0.5.1 的 Windows DLL 与 `refresh_egress.py --serve` 通过 CPA 数据目录中的文件请求/应答信箱通信：`<CODEX_TZ_REFRESH_SOCKET>.ready`、`.request`、`.<nonce>.reply`。只传递随机请求标识与 OK/ERR 状态，**不会把用户请求正文放入信箱**。Windows 主机必须确保共享目录仅允许可信管理员和 CPA 账户读写（NTFS ACL），并用 `python -m pip install tzdata` 安装 IANA 时区数据。Windows 默认数据目录为 `C:\\CLIProxyAPI\\data`，可通过 `CODEX_TZ_STATE_FILE`、`CODEX_TZ_REFRESH_SOCKET`（DLL）和 `CODEX_TZ_STATE`、`CODEX_TZ_REFRESH_SOCKET`（辅助脚本）分别修改。

在 Windows/macOS 上，如果只有动态库而没有运行探测辅助脚本，管理界面的「获取」会提示失败，但通过外部定时任务生成的有效状态文件仍可供时区改写使用。

## 管理界面与数据安全

打开 CPA 管理界面的「Codex Timezone Sync」插件菜单。相关管理 API 必须使用 CPA 管理密钥，浏览器中的静态面板本身不公开统计或请求日志。

**失败请求日志可能包含提示词、文件内容和其他敏感数据。** 插件会保存完整的失败 Codex *请求体*，不保存外围 HTTP Headers、OAuth Cookie 或代理转发封装；但如果请求体中原本带有秘密，仍会一并保存。日志默认在持久化目录中以 `0600` 权限创建，具有大小上限。不要公开数据目录、不要将数据文件加入 Git 或备份到公开存储。

支持的存储位置覆盖：`CODEX_TZ_CONFIG_FILE`、`CODEX_TZ_LOG_FILE`、`CODEX_TZ_STATE_FILE`、`CODEX_TZ_STATS_FILE`、`CODEX_TZ_FAILURE_FILE`。上述配置作用于 CPA 插件进程，与辅助脚本使用的 `CODEX_TZ_STATE` 是不同的环境变量。

## 源码编译与测试

Linux 需要 `cc`、`make` 和 `python3`：

```bash
make clean
make test
python3 test_egress.py
```

包含 40 项 C ABI/管理接口/改写行为测试及 2 项出口检测辅助脚本测试。浏览器测试可按需执行 `python3 test_ui.py`、`python3 test_auth_ui.py`（需 Chrome/Chromium 和 Playwright）。

发布 Workflow 为 Linux amd64 和 arm64 分别构建并运行测试，以 `v0.5.0` 等 Git tag 发布 GitHub Release。插件商店使用的发布文件格式：

```text
codex-timezone-sync_0.5.0_linux_amd64.zip
codex-timezone-sync_0.5.0_linux_arm64.zip
checksums.txt
```

ZIP 根目录仅包含 `codex-timezone-sync.so`，符合 CPA 插件商店直装约定。

## 兼容性与限制

本插件面向 CLIProxyAPI 内置 C ABI 插件宿主，不适用于不支持插件的旧版 CPA。不同 CPA 分支对管理界面、拦截器回调及插件商店的支持可能不同；请使用支持动态插件及对应管理接口的版本。项目尚未覆盖 macOS/Windows 安装包。插件不是地理位置伪装工具，不提供 OAuth 凭据，也不向第三方转发完整请求体。

当前仓库没有明确授予代码再分发许可；使用前请遵守适用法律及上游服务条款。
