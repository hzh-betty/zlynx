# zhttp 5.0 性能优化实施报告

基于 `bac8c5422b08766a21b3a42d2d9f8db2203bc205` 及原分析报告实施。原报告、microbench、probe 和预热脚本均已与提供的 evidence.tar.gz 核对 SHA-256；原目录与基线构建未覆盖。此次允许 ABI 升级，zhttp 和 zhttp_core 的版本/SOVERSION 升为 5.0.0/5，消费者须重新编译。公共业务源码接口保持。

主体核心、HTTP 和分片数对照是在增加分片对齐前测量的；最终对齐布局的性能补测与提交前验证单独列出，不混用样本。

## 修改范围

- 静态缓存维护保守的最早过期时间。无条目过期时跳过全表扫描；清理与目标查询在同一次锁内完成，目标仍检查 TTL。覆盖/淘汰允许下界偏早，最多触发额外扫描；真正清理仍为 O(N)。保留 `now > expires` 的原边界、容量限制和外部快照寿命。
- HTTP 处理器在第一个完整请求时查询连接两端地址与 TLS 信息，保存拥有字符串的连接快照，后续请求复制到各自上下文。查询失败保持空地址；关闭后不再处理请求。
- 非 chunked、允许正文、正文不超过 4096 字节且合计不超过 64 KiB 的内存响应，提交前合并头部与正文，通过原 Connection/ByteStream 同步发送。文件与流仍按原路径发送；保留文件提交前检查、HEAD/204/205/304/Upgrade 和失败关闭行为。
- 三种限流器各用 16 个按 64 字节对齐的分片，key 稳定映射到一个分片，扣减与判定仍由普通 mutex 保证。同 key 仍串行；周期清理只扫描当前分片，显式清理依次持有各分片锁，不同时持有多个锁，也不提供跨分片原子快照。自定义时钟需可并发调用，析构前须结束调用。
- 请求局部 middleware vector 按选中的数量 reserve，空组配置直接短路；执行顺序、逆序 after 和异常语义不变。头部使用 reserve/append；handler、writer、encoder 复用同一响应计划，文件及头部检查仍发生在 commit 前。
- 修复 benchmark 脚本 CRLF 和旧构建选项；真实发送预热请求，与正式测量分开。`--warmup-ms` 向上取整到整秒以兼容本机 wrk。检查 wrk 退出码、有效吞吐、socket error、非成功状态及服务异常退出。增加无新依赖的核心 benchmark。

会话 COW、路由索引、逐字节解析扫描、压缩策略、WebSocket SIMD、zco/znet/allocator 结构与编译优化选项不在本次修改范围。

## 对照方法

原分析的 Clang/Ninja/C++17/Release 配置：`-O3 -DNDEBUG -g`，共享库，网络保持 zmalloc override=ON；核心微基准使用系统 allocator。对照直接使用未修改的原 Release 库，驱动从同一份源码分别按原提交头文件和当前头文件编译，避免公共类布局不一致。

Google Benchmark 复用原驱动，仅改变缓存输入目录到此次 ext4 产物目录。缓存每文件 128 字节、TTL 3600 秒；单线程固定 CPU 2，限流固定 CPU 0–7。前后交错 3 组，每组每项重复 3 次，预热 0.1s、最小测量 0.2s，共 9 个测量值。系统 Benchmark 库仍提示 DEBUG 构建，极短操作数字应谨慎解释。

HTTP：GET /、2 字节 OK、error 日志；服务 CPU 0–7，wrk CPU 8–15，4 客户端线程、256 连接，真实预热 2s、正式测量 5s，独立启动服务，前后交错 3 轮，两种栈 × 1/2/4/8 worker，共 48 轮。静态服务的收益包含缓存和 HTTP 热路径的共同影响，未逐项归因。静态服务另用 4 worker、1024 个预热缓存条目、128 字节响应，同样负载共 6 轮。所有这些轮次均检查错误与正常退出。

以下为本次同条件前后对照，不与原报告中不同时段的数字混算。CV 是样本标准差/均值；P99 是三轮各自 P99 的中位数，不是合并后的分位数。CPU/请求来自正式 wrk 前后 /proc 的进程 CPU 时间差与 wrk 已完成请求数，包含窗口边缘的少量工作。

## 核心结果

| 操作 | 修改前 ns/op | 修改后 ns/op | 前/后耗时比 |
|---|---:|---:|---:|
| `BM_CacheHit/1` | 521.61 | 510.89 | 1.02× |
| `BM_CacheHit/64` | 638.46 | 521.54 | 1.22× |
| `BM_CacheHit/256` | 1174.69 | 538.99 | 2.18× |
| `BM_CacheHit/1024` | 5525.07 | 524.52 | 10.53× |
| `BM_Pipeline/0` | 288.65 | 240.71 | 1.20× |
| `BM_Pipeline/4` | 337.71 | 278.33 | 1.21× |
| `BM_Pipeline/16` | 443.41 | 329.74 | 1.34× |
| `BM_LimiterShared/real_time/threads:1` | 41.78 | 45.45 | 0.92× |
| `BM_LimiterShared/real_time/threads:2` | 220.97 | 54.08 | 4.09× |
| `BM_LimiterShared/real_time/threads:4` | 508.38 | 68.45 | 7.43× |
| `BM_LimiterShared/real_time/threads:8` | 1286.12 | 94.59 | 13.60× |

## HTTP 结果

| 栈/worker | 前 QPS / CV | 后 QPS / CV | QPS 变化 | 前/后 P99 ms | 前/后 CPU µs/请求 |
|---|---:|---:|---:|---:|---:|
| independent/w1 | 39,826 / 3.19% | 59,293 / 3.24% | +48.9% | 8.87 / 5.54 | 24.9 / 16.7 |
| independent/w2 | 58,432 / 6.56% | 90,714 / 9.02% | +55.2% | 9.74 / 4.99 | 33.9 / 22.0 |
| independent/w4 | 63,237 / 8.38% | 108,153 / 11.90% | +71.0% | 12.79 / 7.55 | 55.8 / 34.6 |
| independent/w8 | 65,326 / 4.99% | 108,622 / 9.07% | +66.3% | 13.81 / 9.90 | 86.2 / 57.7 |
| shared/w1 | 40,651 / 6.05% | 59,970 / 5.01% | +47.5% | 9.88 / 7.08 | 24.4 / 16.5 |
| shared/w2 | 60,154 / 8.23% | 93,463 / 1.49% | +55.4% | 10.00 / 5.39 | 32.7 / 21.2 |
| shared/w4 | 67,460 / 6.44% | 114,905 / 5.29% | +70.3% | 11.53 / 8.10 | 51.8 / 32.2 |
| shared/w8 | 67,735 / 5.13% | 116,282 / 3.85% | +71.7% | 12.76 / 8.63 | 84.5 / 53.9 |
| static/w4 | 31,191 / 13.51% | 101,685 / 4.13% | +226.0% | 12.65 / 6.86 | — |

吞吐是本机闭环合成负载结果，P99 存在 coordinated omission。WSL 虚拟拓扑、Windows 宿主频率和背景负载未隔离，短窗口也不代表长期稳定性或生产 SLO。没有修改默认 worker 数，不把更多 worker 的扩展不足归因于某个单一库函数。

## 分片选择与分配复核

分片数只在仓库外复制的头文件中改为 1/4/16/64，其余算法与驱动相同；独立 key 与同 key、1/2/4/8 线程各重复 3 次。这里只测 FixedWindow，并不宣称另外两种算法具有同样倍数的收益。

| 分片数 | 独立 key 1/2/4/8 线程 ns/op | 同 key 8 线程 ns/op |
|---|---:|---:|
| 1 | 48.44 / 446.39 / 535.34 / 1374.45 | 1285.99 |
| 4 | 49.06 / 53.73 / 99.20 / 442.21 | 1258.66 |
| 16 | 44.78 / 53.21 / 67.43 / 99.82 | 1308.81 |
| 64 | 47.75 / 54.07 / 55.62 / 71.60 | 1288.63 |

选用 16 个分片，已取得显著并发收益，避免为这个固定 key 分布扩大到 64 个分片。同 key 无扩展收益，原 Google Benchmark 单线程观测到约 9% 的耗时增加（41.78→45.45ns）；独立驱动单线程分别观测到约 9%（thread0）和约 19%（shared）。实现新增了分片哈希，同 key 8 线程独立驱动观测到约 12% 的耗时增加，短测量仍受调度波动影响；这是换取不同 key 并行度的代价，不能宣称所有限流负载都变快。哈希碰撞、高基数清理与热点 key 仍可限制性能。本机 x86_64 上三种限流器实例均从 160 增至 1720 字节，固定增加 1560 字节/实例；每 key 的状态仍按需分配。

固定循环 probe 拦截普通 operator new/new[]，5 次重复；不是所有 malloc 或 zmalloc 内部行为：

| 操作 | 前 new/次 | 后 new/次 |
|---|---:|---:|
| cache/1 | 3 | 3 |
| cache/1024 | 3 | 3 |
| pipeline/0 | 4 | 4 |
| pipeline/16 | 9 | 5 |
| encode/1 | 11 | 3 |

无 Google Benchmark 依赖的仓库核心 benchmark 另作复核，原始 CSV 为 `core-before-*.csv` / `core-after-*.csv`。不同驱动的耗时单独解释，不合并样本。限流表中的 Google Benchmark 多线程 real_time 与独立驱动的墙钟/总操作数口径不同，不直接用 1/ns 换算总吞吐。独立驱动 8 线程、独立 key 的总操作耗时比为 13.93×。

| 独立驱动：限流负载 | 前 ns/总操作 | 后 ns/总操作 | 前/后耗时比 |
|---|---:|---:|---:|
| limiter/different/1 | 45.49 | 49.72 | 0.91× |
| limiter/different/2 | 186.62 | 25.78 | 7.24× |
| limiter/different/4 | 126.35 | 16.86 | 7.49× |
| limiter/different/8 | 168.42 | 12.09 | 13.93× |
| limiter/same/1 | 44.81 | 53.26 | 0.84× |
| limiter/same/2 | 208.67 | 117.00 | 1.78× |
| limiter/same/4 | 129.14 | 130.08 | 0.99× |
| limiter/same/8 | 161.71 | 181.59 | 0.89× |

## 分片对齐补充验证

上述核心、HTTP 和分片数对照均为增加分片对齐前的数据，保留原始测量，不与补测合并。补测后采用 `struct alignas(64) Shard`，保留 16 个分片和原算法。

原分片对照的 `thread0`–`thread7` 在本机 16 分片下依次映射到 5、11、6、8、2、12、7、9，已经没有分片碰撞。因此 64 分片的收益不能解释为减少了这些 key 的锁碰撞；本轮只验证布局变更的实际效果，未测量缓存一致性事件。

补测使用相同 Clang/C++17/Release 编译选项和 CPU 0–7，5 组轮换版本顺序。复用原 Google Benchmark 驱动，预热 0.1s、最小测量 0.3s；另用无 Benchmark 依赖的独立驱动复核，每线程预热 100ms、同步测量 400ms、每 1024 次操作检查时间，逐轮验证允许数等于总操作数。下表来自独立驱动，单位为墙钟时间/总操作数，列内为均值 / 样本 CV：

| FixedWindow 负载 | 未对齐 16 分片 ns / CV | 未对齐 64 分片 ns / CV | 对齐 16 分片 ns / CV |
|---|---:|---:|---:|
| 1 线程，thread0 | 49.401 / 0.63% | 49.445 / 1.42% | 49.230 / 0.79% |
| 8 线程，thread0–thread7 | 12.514 / 4.98% | 8.715 / 1.67% | 7.921 / 0.87% |
| 8 线程，8 个无碰撞 IPv4 key | 9.225 / 5.56% | 7.855 / 1.41% | 7.098 / 0.28% |
| 8 线程，同 key | 158.317 / 2.24% | 162.450 / 2.48% | 166.155 / 0.92% |

相对未对齐 16 分片，两组独立 key 负载耗时分别降低约 37% 和 23%，同 key 约慢 5%，单线程没有清晰改善。IPv4 key 是筛选为无分片碰撞的合成输入，不代表真实客户端分布。Google Benchmark 也确认独立 key 并发收益，并观测到同 key 约 7% 的耗时增加；其多线程 real_time 口径与上表不同，不合并样本。

本机对齐后的 FixedWindow/SlidingWindow/TokenBucket 实例分别为 2176/2176/2112 字节；未对齐 16 分片均为 1720 字节，64 分片均为 6712 字节。这只是实例固定体积，不是完整 RSS。消费者必须使用最终头文件重新编译，不能混用对齐前后的公共类布局。

仓库外对齐原型通过现有 15 个限流单元测试，以及三种算法的 TSan 并发额度与清理压力测试。性能只验证 FixedWindow；其他算法、高基数清理、偏斜 key、其他 CPU 和启用限流的 HTTP 端到端收益未测量。默认 GET 基准没有启用限流，不能把局部收益加到其 QPS 上。

原始补测、驱动、编译命令和完整复核报告位于 `/home/betty/zhttp-review-20261011-gl9dnzyn`。本次 GET 的已有数据中，4 worker 的 CPU/请求比 8 worker 少约 40%，吞吐均值差仅 0.43%/1.20%，小于轮间波动；这可用于该负载的配置选择，不作为其他业务的默认线程数依据。

## 正确性及系统调用

- Release：33/33 zhttp CTest 目标通过，含 HTTP/TLS/WebSocket 集成；新增覆盖 TTL 精确边界、错开到期、淘汰后下界、并发清理与快照寿命、不同 key 与同 key 的额度、各分片回收、地址查询失败和拥有权、小响应阈值、HEAD/无正文/Upgrade、部分写入及失败完成通知。
- ASan/UBSan：23/23 核心测试目标通过，独立 Debug 构建、override=OFF。未覆盖网络协程运行时；不宣称整个运行时通过 Sanitizer。
- TSan：独立限流器压力驱动通过（8 请求线程及并发清理线程，三种算法，固定时钟并检查允许次数）；独立 StaticResourceStore 的 5 个测试通过。未对 zco/TLS/完整服务运行 TSan。
- benchmark 协调进程的 7 类情形通过；真实 wrk 的 500ms 预热（实际向上取整为 1s）冒烟通过；bash -n、Python 语法检查及 git diff --check 通过。
- 临时前缀安装后，find_package(zhttp 5) 的 core/full 消费者编译并运行通过；SONAME 为 libzhttp.so.5 与 libzhttp_core.so.5。

strace 用父进程启动，`-f -c -b execve` 在客户端 exec 时脱离，只跟踪服务/协调进程。每端 1 worker、8 连接、1s、无预热，独立于吞吐基准；统计见 `strace-before.txt` / `strace-after.txt`。测试中的 RecordingStream 还验证：多个请求只查询两端地址各一次，2 字节内存响应在完整传输可一次写完时只调用一次 write_some，部分传输仍能正确继续或失败关闭。strace 的吞吐与耗时百分比不用于性能结论。

| 系统调用 | 修改前次数 | 修改后次数 |
|---|---:|---:|
| getpeername | 1414 | 8 |
| getsockname | 1415 | 9 |
| sendto | 2828 | 1832 |

两次跟踪处理的请求数不同，绝对调用数不能直接作为降低百分比。修改前 sendto 恰为 getpeername 的两倍；修改后地址查询各为连接数（8），getsockname 另含一次监听地址查询。

提交前从各批次的 Git 暂存树导出源码快照，分别完成对应构建和测试。最终对齐布局通过全部 33 个 zhttp Release CTest、23 个核心 ASan/UBSan CTest、三种限流算法的 TSan 压力，以及真实 wrk 双栈预热冒烟和核心限流基准。临时安装后的 `find_package(zhttp 5)` core/full 消费者均编译运行通过，额外检查工厂创建对象的 64 字节对齐，实际实例大小为 2176/2176/2112 字节，两个共享库 SONAME 均为 5。网络协程运行时仍未做 Sanitizer 验证。

分批验证的暂存树、补丁、命令与日志位于 `/home/betty/zhttp-commits-20261011-qw6e06nh`。full 消费者首次启动漏配 README 要求的动态库搜索路径，未找到临时前缀中的 `libzlog.so.2`；按文档设置该前缀的 `LD_LIBRARY_PATH` 后通过，首次失败日志保留。

## 产物与复现

完整产物目录：`/home/betty/zhttp-optimization-20261011`。包含环境、configure/build/test 日志、原始每轮 wrk、/proc 快照、JSON/CSV、分配 probe、分片对照、TSan 驱动、安装消费驱动、`summary.json`、源码快照及变更补丁。原始源码驱动来自原 evidence 归档，新增驱动不覆盖原分析目录。

早期缓存方案多读一次时钟并多持一次锁，单条目结果略慢，已修正并重新完整对照。试验数据及中断的首轮 HTTP 运行保存在 `exploratory-before-cache-lock-refinement/`，未计入此处结果。初次 Writer 编译缺少新 overload 的 friend 声明，已补齐；该失败日志保留，后续完整构建通过。静态服务驱动首版漏设 GET，在预热校验中失败；同时修正尚未执行对照的核心 pipeline 驱动并加入未命中即失败的校验，再运行剩余有效轮次；安装消费者首版用 Builder 的端口 0，被已有配置校验拒绝，改用支持临时端口的 HttpServer 构造后验证。相关失败日志均保留。

项目内手动复现：

```bash
cmake -S . -B build/zhttp-opt -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g" \
  -DBUILD_TESTING=ON -DBUILD_SHARED_LIBS=ON \
  -DZLYNX_BUILD_PERF_TESTS=ON -DZLYNX_USE_ZMALLOC_OVERRIDE=ON
cmake --build build/zhttp-opt -j 2
ctest --test-dir build/zhttp-opt -R '^zhttp\.' --output-on-failure
build/zhttp-opt/zhttp/tests/zhttp_benchmark --mode all --threads 4 \
  --wrk-threads 4 --wrk-connections 256 --warmup-ms 2000 \
  --wrk-duration 5s --wrk-arg --latency
taskset -c 2 build/zhttp-opt/zhttp/tests/zhttp_core_benchmark cache
taskset -c 2 build/zhttp-opt/zhttp/tests/zhttp_core_benchmark pipeline
taskset -c 2 build/zhttp-opt/zhttp/tests/zhttp_core_benchmark encode
taskset -c 0-7 build/zhttp-opt/zhttp/tests/zhttp_core_benchmark limiter
```

完整对照由产物中的 compare.py / summarize.py 复现；需保留原提交的 Release 库、baseline-source 原头文件及对应编译的驱动。该脚本会覆盖此次目录的同名结果，复跑前复制产物目录并调整旧构建路径。perf preset 仍为 RelWithDebInfo，与上述 Release 配置不可混用。
