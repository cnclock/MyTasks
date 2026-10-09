# MyTasks

轻量级 Windows 服务：在计算机不使用时自动执行关机（或其它命令），并支持服务启动后按延时运行多个程序。

- 运行账户：**LocalSystem**
- 启动类型：**自动**
- 配置文件：`config.ini`
- 日志文件：默认 `MyTasks.log`（与 EXE 同目录，也可在安装时指定）

---

## 功能概览

| 功能 | 说明 |
|------|------|
| 登录界面触发 | 出现登录/锁定界面时执行动作（锁定、注销、切换用户等） |
| 闲置超时 | 无键鼠操作达到设定分钟数后执行动作 |
| 定时执行 | 每天在指定时刻（可多个）执行动作 |
| 启动延时任务 | 服务启动后，按各自延时依次启动多个程序 |

上述「动作」统一由 `config.ini` 中的 `ScriptPath` / `ScriptArgs` 决定（可以是 `shutdown.exe`，也可以是脚本）。

---

## 环境要求

- Windows 7 及以上（x64）
- 编译需要 **MinGW g++**（本机若已安装 `D:\MinGW\bin`，`build.bat` 会自动加入 PATH）
- **安装 / 卸载服务必须使用管理员权限**

---

## 目录结构

```text
MyTasks/
├── src/mytasks.cpp      # 服务源码
├── config.ini           # 配置模板（编译时复制到 build\）
├── scripts/             # 示例脚本
├── build.bat            # 编译
├── install.bat          # 安装服务（可跟 -config / -log）
├── uninstall.bat        # 卸载服务
├── README.md
└── build/               # 编译输出（安装实际使用这里）
    ├── MyTasks.exe
    ├── config.ini
    ├── MyTasks.log      # 运行后生成
    └── scripts/
```

---

## 编译

在项目根目录执行：

```bat
build.bat
```

成功后生成：

- `build\MyTasks.exe`
- `build\config.ini`（从根目录复制）
- `build\scripts\*.bat`

日常使用、安装服务时，以 **`build\` 目录** 为准。

---

## 安装

### 1. 默认安装（配置与日志均在 EXE 同目录）

以**管理员**打开命令提示符或 PowerShell，进入项目目录后执行：

```bat
install.bat
```

或：

```bat
build\MyTasks.exe install
```

效果：

- 注册服务名：`MyTasks`
- 启动类型：自动
- 账户：LocalSystem
- 配置文件：`build\config.ini`
- 日志文件：`build\MyTasks.log`
- 安装后立即启动服务

### 2. 自定义配置文件 / 日志路径

安装时用参数指定（路径会写入服务的启动命令行，服务运行时一直有效）：

```bat
build\MyTasks.exe install -config D:\Data\MyTasks\config.ini -log D:\Data\MyTasks\MyTasks.log
```

也可：

```bat
install.bat -config D:\Data\MyTasks\config.ini -log D:\Data\MyTasks\MyTasks.log
```

说明：

| 参数 | 含义 | 不指定时的默认值 |
|------|------|------------------|
| `-config` | 配置文件完整路径 | `EXE所在目录\config.ini` |
| `-log` | 日志文件完整路径 | `EXE所在目录\MyTasks.log` |

相对路径相对于 **EXE 所在目录** 解析。  
配置文件内的相对路径（如脚本路径）相对于 **config.ini 所在目录** 解析。

安装前请先把 `config.ini`（及需要的脚本）放到 `-config` 指向的位置。

### 3. 查看当前解析到的路径

```bat
build\MyTasks.exe paths
build\MyTasks.exe paths -config D:\Data\MyTasks\config.ini -log D:\Data\MyTasks\MyTasks.log
```

### 4. 卸载

管理员执行：

```bat
uninstall.bat
```

或：

```bat
build\MyTasks.exe uninstall
```

### 5. 常用服务命令

```bat
sc query MyTasks
sc stop MyTasks
sc start MyTasks
```

修改了 `[Startup]` 延时任务后，需要**重启服务**才会重新加载。  
其它多数配置（闲置、定时、登录界面开关等）会在下一轮检测周期重新读取。

---

## 配置说明（config.ini）

### [Settings] — 触发后执行的动作

```ini
[Settings]
ScriptPath=C:\Windows\System32\shutdown.exe
ScriptArgs=/s /t 60 /c MyTasks-auto
WorkingDirectory=
```

| 项 | 说明 |
|----|------|
| `ScriptPath` | 要运行的程序或脚本。支持 `.exe` / `.bat` / `.cmd` / `.ps1`。可为绝对路径，或相对 **本 config.ini 所在目录** |
| `ScriptArgs` | 传给程序的参数 |
| `WorkingDirectory` | 工作目录；留空则使用脚本所在目录 |

示例：改用脚本关机

```ini
ScriptPath=scripts\on_logoff.bat
ScriptArgs=
```

**注意：** 默认配置会调用 `shutdown.exe`，测试时请先改成安全脚本（如 `scripts\dryrun.bat`），避免误关机。

---

### [LoginScreen] — 登录 / 锁定界面

出现凭据界面（登录、锁定等）时执行 `[Settings]` 中的动作。同一轮停留在登录界面只触发一次，回到桌面后重新武装。

```ini
[LoginScreen]
Enabled=1
OnLock=1
OnLogoff=1
OnDisconnect=1
PollDetect=1
```

| 项 | 说明 |
|----|------|
| `Enabled` | 总开关 |
| `OnLock` | Win+L / 会话锁定 |
| `OnLogoff` | 用户注销，或关机流程中会话已注销后停在登录界面 |
| `OnDisconnect` | 切换用户、控制台断开等 |
| `PollDetect` | 周期性检测是否已在登录界面，用于补全事件遗漏 |

不会把 **UAC 提权框** 当成登录界面。

「有程序阻止关机」时通常仍是桌面上的阻止界面，不会立刻当登录界面处理；只有最终停在登录界面时才会按本节约定触发。

---

### [Idle] — 闲置超时

```ini
[Idle]
Enabled=1
IdleMinutes=30
CheckIntervalSeconds=30
CountLoginScreen=1
```

| 项 | 说明 |
|----|------|
| `Enabled` | 是否启用闲置触发 |
| `IdleMinutes` | 无键盘/鼠标输入多少分钟后执行动作 |
| `CheckIntervalSeconds` | 检测间隔（秒）。建议 15–60，以免错过定时整点 |
| `CountLoginScreen` | 在登录/锁定界面是否也累计闲置时间 |

闲置判定在用户会话中查询真实键鼠空闲时间（服务本身运行在 Session 0，不能直接用本进程的 `GetLastInputInfo`）。

触发一次后，需有新的输入活动才会再次触发。

---

### [Schedule] — 每天定时

```ini
[Schedule]
Enabled=0
Times=23:00,12:30
OnlyWhenIdle=0
```

| 项 | 说明 |
|----|------|
| `Enabled` | 是否启用 |
| `Times` | 本地时间，24 小时制 `HH:MM`，多个用逗号分隔 |
| `OnlyWhenIdle` | 为 `1` 时，到点且闲置达到 `IdleMinutes` 才执行 |

每个时刻每天最多触发一次。

---

### [Startup] — 服务启动后延时运行程序

与关机动作无关：服务启动后按各自延时启动最多 **16** 个程序。

```ini
[Startup]
Enabled=1

Task1_DelaySeconds=60
Task1_Path=scripts\startup_task1.bat
Task1_Args=
Task1_WorkingDirectory=

Task2_DelaySeconds=300
Task2_Path=C:\Tools\something.exe
Task2_Args=/quiet
Task2_WorkingDirectory=
```

| 项 | 说明 |
|----|------|
| `Enabled` | 是否启用启动任务 |
| `TaskN_DelaySeconds` | 相对**服务启动时刻**的延时（秒） |
| `TaskN_Path` | 程序/脚本路径；**留空则忽略该任务** |
| `TaskN_Args` | 参数 |
| `TaskN_WorkingDirectory` | 工作目录，可留空 |

任务按延时排序执行；启动后不等待进程结束，以免影响后续任务计时。  
修改本段配置后需重启服务。

---

## 使用与调试命令

以下命令在 `build\` 目录下执行（或写完整路径）。调试命令也可附加 `-config` / `-log`。

| 命令 | 作用 |
|------|------|
| `MyTasks.exe install [-config PATH] [-log PATH]` | 安装并启动服务 |
| `MyTasks.exe uninstall` | 卸载服务 |
| `MyTasks.exe paths` | 显示解析后的配置/日志路径 |
| `MyTasks.exe test` | **立即执行一次** `[Settings]` 动作（若配置了 shutdown 会真关机） |
| `MyTasks.exe idle` | 查看当前闲置秒数与阈值 |
| `MyTasks.exe startup` | 列出启动延时任务 |
| `MyTasks.exe startup now` | 忽略延时，立刻跑一遍所有启动任务 |

建议测试流程：

1. 把 `ScriptPath` 临时改为 `scripts\dryrun.bat`
2. 执行 `MyTasks.exe test`，确认 `dryrun.log` 有记录
3. 执行 `MyTasks.exe startup now`，确认启动任务脚本有日志
4. 再改回正式关机命令并安装服务

---

## 推荐工作流

1. `build.bat` 编译  
2. 编辑 `build\config.ini`（或准备好自定义目录下的配置）  
3. 用 `dryrun` 测通后再改正式 `ScriptPath`  
4. 管理员安装：
   ```bat
   build\MyTasks.exe install
   ```
   或指定路径：
   ```bat
   build\MyTasks.exe install -config D:\Data\MyTasks\config.ini -log D:\Data\MyTasks\MyTasks.log
   ```
5. 查看日志确认服务已启动、配置已加载  
6. 需要时用 `sc stop/start MyTasks` 重启服务

---

## 日志

- 默认：`EXE同目录\MyTasks.log`
- 安装时 `-log` 指定的路径
- 内容包括：服务启停、会话事件、闲置/定时/登录界面触发、程序启动结果等

若日志目录不存在，服务会尝试自动创建。

---

## 注意事项

1. **LocalSystem** 下运行的脚本没有当前用户桌面交互环境；适合关机、清理、无界面工具，不适合依赖用户界面的程序。  
2. 默认 `ScriptPath` 为关机命令，测试务必先改掉。  
3. 若本机曾安装旧服务名 `MyShutdown`，请先卸载：
   ```bat
   sc stop MyShutdown
   sc delete MyShutdown
   ```
4. 覆盖更新 EXE 时：先 `sc stop MyTasks`，替换 `build\MyTasks.exe`，再 `sc start MyTasks`。若安装参数（`-config`/`-log`）有变，需重新 `install`。  
5. `CheckIntervalSeconds` 不宜过大，否则可能错过 `Schedule` 的某一分钟。

---

## 许可与用途

供本机自动化运维使用。请确认关机/脚本行为符合你的安全策略后再部署到生产环境。
