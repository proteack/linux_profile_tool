# USE Method: Linux Performance Analysis Tool

基于 [Brendan Gregg 的 USE Method（Utilization, Saturation, Errors）](https://www.brendangregg.com/USEmethod/use-linux.html) 检查清单的零依赖 Linux 性能分析工具，内置 **perf 采样分析** 与 **火焰图生成** 能力。

自动执行清单中的所有命令，解析输出，以结构化彩色结果呈现，并基于内置经验阈值标记系统瓶颈。

## 目录

- [快速开始](#快速开始)
- [USE 方法简介](#use-方法简介)
- [命令参考](#命令参考)
- [资源与指标](#资源与指标)
  - [CPU](#cpu)
  - [内存](#内存)
  - [网络](#网络)
  - [存储](#存储)
  - [软件资源](#软件资源)
- [输出说明](#输出说明)
  - [终端输出](#终端输出)
  - [JSON 输出](#json-输出)
- [阈值规则](#阈值规则)
- [编译与安装](#编译与安装)
- [常见问题](#常见问题)

## 快速开始

```bash
# 编译
make

# 运行完整检查（推荐）
./use-linux-perf

# 60 秒快速检查（Netflix 清单）
./use-linux-perf --mode quick

# 只看有问题的资源
./use-linux-perf --mode summary

# Perf 采样分析（需 root/sudo）
sudo ./use-linux-perf --perf --time 30

# 采样 + 生成火焰图 SVG
sudo ./use-linux-perf --perf --time 60 --flame
```

无需 root 权限即可运行大部分检查。部分命令（如 `dmesg`、`smartctl`）在普通用户下可能输出受限，不影响核心功能。

## USE 方法简介

USE 方法由 Brendan Gregg 提出，核心思想是**对每个资源检查三个维度**：

| 维度 | 含义 | 典型问题 |
|------|------|---------|
| **U**tilization | 资源忙闲比例（%） | CPU 100% → 计算瓶颈 |
| **S**aturation | 等待队列长度 | 高 run queue → CPU 过载 |
| **E**rrors | 错误事件计数 | 网卡 error 增长 → 硬件故障 |

三个指标结合可以快速定位瓶颈类型和位置，避免盲目猜测。

## 命令参考

```
Usage: ./use-linux-perf [options]

Options:
  --mode MOD          执行模式: full | quick | summary (默认: full)
  --resource LIST     资源过滤: cpu,memory,network,storage,software (默认: all)
  --output FMT        输出格式: terminal | json (默认: terminal)
  --perf              Perf 采样分析模式（需 root 或 CAP_PERFMON）
  --time SECONDS      采样时长（默认: 30，仅 --perf 模式生效）
  --pid PID           绑定到指定进程 PID（默认: 全系统）
  --freq HZ           采样频率 Hz（默认: 99，仅 --perf 模式生效）
  --flame             生成 CPU 火焰图 SVG（仅 --perf 模式生效）
  --flame-output PATH 火焰图输出路径（默认: perf_flame_时间戳.svg）
  --help              显示帮助
```

### 执行模式

| 模式 | 说明 |
|------|------|
| `full` | 运行所有检查。覆盖 CPU、内存、网络、存储 I/O、存储容量、软件资源 |
| `quick` | 60 秒快速检查（Netflix 生产环境清单）。包含 uptime、dmesg、vmstat、mpstat、pidstat、iostat、free、sar、top |
| `summary` | 与 full 相同，但只显示有问题的资源（OK 的隐藏） |

### 资源过滤

`--resource` 接受逗号分隔的资源名称，只检查指定的资源：

```bash
./use-linux-perf --resource cpu
./use-linux-perf --resource cpu,memory
./use-linux-perf --resource network,storage
```

### 输出格式

**terminal**（默认）：带 ANSI 颜色的表格输出，适合终端查看。

**json**：结构化 JSON 输出，适合程序解析和集成：

```bash
./use-linux-perf --output json
```

### Perf 采样模式

`--perf` 模式使用 Linux `perf` 工具对系统或进程进行 CPU 采样分析，自动识别热点函数、检测异常，并可生成火焰图 SVG。

```bash
# 全系统采样 30 秒
sudo ./use-linux-perf --perf

# 采样 60 秒，针对特定进程
sudo ./use-linux-perf --perf --pid 1234 --time 60

# 采样并生成火焰图
sudo ./use-linux-perf --perf --time 30 --flame

# 指定火焰图输出路径
sudo ./use-linux-perf --perf --flame --flame-output /tmp/flame.svg
```

> **权限要求**：`perf record` 需要 root 或 `CAP_PERFMON` 能力。

#### 热点函数报告

采样完成后输出 Top CPU 消耗者表格，包含函数名、所属进程、CPU 占比、内核/用户态标识：

```
--- Top CPU Consumers ---
Kernel: 30.2%  User: 60.1%  Idle: 9.7%

  Overhead  Process            Symbol                                   Location
  ------------------------------------------------------------------------------------------
  25.4%    swapper            native_safe_halt                         [kernel]
  15.3%    mysqld             join_record                              [user]
  12.1%    mysqld             do_syscall_64                            [kernel]
   ...
```

#### 异常自动检测

分析报告时会自动识别以下异常模式并高亮标记：

| 异常类型 | 触发条件 | 示例输出 |
|---------|---------|---------|
| **HIGH CPU** | 单函数 CPU > 20% | `HIGH CPU: 'join_record' in mysqld consumes 35% CPU` |
| **内核瓶颈** | 内核态占比 > 70% | `HIGH KERNEL TIME: 82% of CPU spent in kernel` |
| **锁竞争** | `lock/spin/mutex` 等 > 5% | `LOCK CONTENTION: 'spin_lock' at 12%` |
| **频繁分配** | `alloc/malloc/free` 等 > 5% | `ALLOC HEAVY: 'malloc' at 8%` |
| **大量拷贝** | `memcpy/copy_user` 等 > 5% | `DATA COPY: 'copy_user_enhanced' at 15%` |
| **页管理异常** | `tlb/page_fault` 等 > 5% | `PAGE MANAGEMENT: 'do_page_fault' at 6%` |
| **系统空闲** | swapper > 70% | `IDLE: swapper accounts for 85% of samples` |

#### 火焰图

使用 `--flame` 生成 CPU 火焰图 SVG，可直接在浏览器中打开：

- **矩形宽度** ∝ CPU 占用比例
- **颜色编码**：暖色(橙/黄) = 用户态，蓝色 = 内核态，绿色 = 空闲
- **鼠标悬停**：高亮当前帧 + 提示框显示完整函数名、采样数、占比
- **自底向上**：底部为根函数（入口），顶部为叶子函数（最内层调用）

## 资源与指标

### CPU

| 指标 | 来源 | 说明 |
|------|------|------|
| **Utilization** | `vmstat 1 2` | us + sy + st（用户 + 系统 + 偷取时间），不含 idle 和 iowait |
| **Saturation** | `vmstat 1 2` | run queue（r 列）超过 CPU 核心数说明 CPU 不够用 |
| **Errors** | `dmesg` | 搜索 Machine Check Exception、thermal throttling、soft/hard lockup 等 |

**解读**：
- Utilization < 70%：正常
- 70-90%：注意，可能有突发瓶颈
- > 90%：CPU 瓶颈，考虑扩容或优化
- runq > CPU 核心数：任务在排队，CPU 饱和

### 内存

| 指标 | 来源 | 说明 |
|------|------|------|
| **Utilization** | `free -m` | 已用内存 / 总内存（含 buffer/cache） |
| **Saturation** | `vmstat 1 2` | swap in/out（si/so）> 0 表示内存压力 |
| **Errors** | `dmesg` | 搜索 OOM Killer、page allocation failure 等 |

**解读**：
- Utilization < 80%：正常
- 80-95%：内存紧张
- > 95%：接近 OOM 风险
- swap si/so > 0：系统正在换页，性能将显著下降

### 网络

对每个非 loopback 接口报告：

| 指标 | 来源 | 说明 |
|------|------|------|
| **Utilization** | `/proc/net/dev`, `ip -s link` | RX/TX 累计字节数（需结合链路速率判断） |
| **Saturation** | `/proc/net/dev` | dropped packets 反映接收队列溢出 |
| **Errors** | `/proc/net/dev` | RX/TX 错误计数 |

额外包含 TCP 统计信息（retransmits, bad segments 等），来自 `netstat -s`。

### 存储

对每个活动块设备报告：

| 指标 | 来源 | 说明 |
|------|------|------|
| **Utilization** | `iostat -xz 1 2` | %util — 设备忙闲比 |
| **Saturation** | `iostat -xz 1 2` | avgqu-sz（平均队列长度）、await（平均 I/O 等待时间） |
| **Errors** | `dmesg`, `smartctl` | I/O 错误事件 |

对每个挂载点（排除 tmpfs、squashfs、snap）报告容量：

| 指标 | 来源 | 说明 |
|------|------|------|
| **Capacity** | `df -h` | 已用空间百分比 |

**解读**：
- %util > 60%：磁盘繁忙
- %util > 85%：磁盘瓶颈
- avgqu-sz > 1：I/O 开始排队
- await > 10ms：磁盘响应慢
- await > 25ms：磁盘性能严重下降

### 软件资源

| 指标 | 来源 | 说明 |
|------|------|------|
| **Tasks** | `ps`, `/proc/sys/kernel/threads-max` | 当前进程数 / 系统上限 |
| **Load Average** | `/proc/loadavg` | 1/5/15 分钟平均负载 |
| **Uptime** | `/proc/uptime` | 系统运行时间 |

> **注意**：Linux 的 load average 包含 uninterruptible I/O 任务，不单纯代表 CPU 负载。CPU 饱和度请参考 vmstat 的 r 列。

## 输出说明

### 终端输出

```
=== USE Method: Linux Performance Analysis ===
Timestamp: 2026-05-24 17:00:00

-------------------------- CPU ---------------------------
  Utilization   2.0%              OK
                us=2% sy=0% id=98% wa=0% (CPUs=8)
  Saturation    runq=0/8          OK
                runnable threads = 0
  Errors        0                 OK
                No CPU errors in dmesg

------------------------- Memory --------------------------
  Utilization   26.5%             OK
                used=2102M / total=7927M avail=5497M swap=0M/2047M
  Saturation    swap=0 KB/s       OK
  Errors        0                 OK

=======================  Summary  ========================
Summary: 0 issue(s) across 9 resource(s) (23 metrics)
  All OK!
```

颜色含义：
- **绿色** OK — 指标正常
- **黄色** WARNING — 需要关注
- **红色** DANGER — 需要立即处理
- **蓝色** INFO — 参考信息

### JSON 输出

```json
{
  "timestamp": "2026-05-24 17:00:00",
  "issues": 2,
  "resources": [
    {
      "name": "CPU",
      "metrics": [
        {
          "name": "Utilization",
          "value": "2.0%",
          "status": "ok",
          "detail": "us=2% sy=0% id=98% wa=0% (CPUs=8)",
          "command": "vmstat 1 2"
        }
      ]
    }
  ]
}
```

每个 metric 的状态字段取值：`ok` | `warning` | `danger` | `info` | `na`。

## 阈值规则

| 指标 | Warning | Danger |
|------|---------|--------|
| CPU utilization | > 70% | > 90% |
| CPU runq / core ratio | > 1.0 | > 2.0 |
| Memory utilization | > 80% | > 95% |
| Swap si/so | > 0.1 KB/s | > 100 KB/s |
| Disk %util | > 60% | > 85% |
| Disk avgqu-sz | > 1.0 | > 8.0 |
| Disk await | > 10 ms | > 25 ms |
| Network drops | > 0 | > 1000 |
| Network errors | > 0 | > 100 |
| FD usage | > 70% | > 90% |
| Task usage | > 70% | > 90% |
| Filesystem capacity | > 80% | > 95% |

## 编译与安装

### 依赖

- C++11 编译器（g++ 4.8+ 或 clang 3.3+）
- `make`
- 系统命令：`vmstat`、`mpstat`、`iostat`、`free`、`ip`（部分来自 sysstat 包）
- `--perf` 模式需要：`perf`（来自 linux-tools 包）及 root/CAP_PERFMON 权限

### 编译

```bash
cd use-linux-perf
make
```

### 安装系统命令（Debian/Ubuntu）

```bash
sudo apt-get install sysstat smartmontools linux-tools-$(uname -r)
```

### 交叉编译（嵌入式 Linux）

修改 Makefile 中的 CXX 为交叉编译器：

```makefile
CXX = arm-linux-gnueabihf-g++
```

然后：

```bash
make
```

## 常见问题

**Q: 为什么需要 `vmstat` 和 `mpstat`？**
A: 这是 USE 清单的核心工具。如果没有，安装 sysstat 包即可。

**Q: 部分指标显示 N/A 或 0？**
A: 可能原因：命令不存在、权限不足（如 dmesg 需要 CAP_SYSLOG）、或设备无活动。

**Q: JSON 输出可以用来做什么？**
A: 可以对接监控系统、写入日志、或用于自动化巡检脚本。

**Q: 工具会修改系统吗？**
A: 不会。工具只读取 `/proc`、`/sys` 和运行标准命令，是**完全只读**的。

**Q: iostat 显示 avgqu-sz 很高但 %util 很低？**
A: 这通常是 iostat 累积了自启动以来的数据。工具采样 2 次取最后一次，但如果系统刚启动不久或设备有偶发 I/O 突发，平均值可能偏高。可以多运行几次观察变化。

**Q: 网络 Utilization 为什么显示 "cumulative since boot"？**
A: 工具读取的是 `/proc/net/dev` 的累计计数器。要计算实时带宽利用率需要连续采样（与 `sar -n DEV 1` 类似的功能），后续版本会支持。

**Q: `--perf` 模式需要什么权限？**
A: 需要 root 或 `CAP_PERFMON` 能力。普通用户运行会提示 "No perf data collected"。可以用 `sudo` 运行。

**Q: 火焰图 SVG 打开后是空白？**
A: SVG 文件兼容所有现代浏览器（Chrome、Firefox、Edge）。如果空白，可能是采样期间没有 CPU 活动（比如系统空闲），可以尝试使用 `--pid` 锁定一个活跃进程。

**Q: 采样多久比较合适？**
A: 默认 30 秒通常足够。对于偶发性能问题，建议 60-120 秒。频率 99 Hz 是标准配置，避免与系统定时器产生谐波干扰。
