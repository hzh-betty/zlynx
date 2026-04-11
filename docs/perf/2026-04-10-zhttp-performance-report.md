# zhttp 性能测试与优化报告

## 1. 目标与范围

本次工作围绕 `zhttp` 做两类性能分析与优化，并输出可复现实验结果：

1. 使用 `perf record/report` 定位 CPU 热点函数，并针对热点路径做优化。
2. 使用 `valgrind --tool=cachegrind` 分析缓存与分支行为，针对热路径的访存模式做优化。

压测入口使用 `zhttp/tests/benchmark/zhttp_benchmark.cc`，由内置 HTTP server 配合 `wrk` 产生负载；同时覆盖 `independent` 与 `shared` 两种栈模式。

## 2. 环境与方法

- 日期：2026-04-10
- 主工作目录：`/home/betty/repositories/zlynx`
- 基线 worktree：`/home/betty/repositories/zlynx/.worktrees/zhttp-perf-baseline`
- 主构建目录：`/home/betty/repositories/zlynx/build-zhttp-perf`
- 基线构建目录：`/home/betty/repositories/zlynx/.worktrees/zhttp-perf-baseline/build-zhttp-perf`
- 构建类型：`RelWithDebInfo`
- 关键 CMake 选项：
  - `-DENABLE_TESTS=ON`
  - `-DZLYNX_USE_ZMALLOC_OVERRIDE=OFF`

构建命令：

```bash
cmake -S . -B build-zhttp-perf -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_TESTS=ON \
  -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF

cmake --build build-zhttp-perf -j --target \
  zhttp_benchmark \
  http_request_test \
  http_response_test \
  http_parser_test \
  http_parser_detailed_test \
  router_test \
  router_detailed_test \
  request_body_middleware_test
```

说明：

- 仓库内新增了统一脚本 `zhttp/tests/benchmark/run_zhttp_wrk_perf.sh`，用于串行执行 `baseline / perf / valgrind`。
- `cachegrind` 默认使用 `ZHTTP_PERF_SCALE_PCT=25` 缩放负载，避免采样时间过长。
- `cachegrind` 通过 `--trace-children=yes` 抓取 server 子进程，并选择最大的 `cachegrind.out.*` 作为最终 server 侧缓存报告。
- 由于沙箱内本地 socket/listen 会被限制，所有 benchmark / perf / valgrind 均在沙箱外执行。

## 3. 代码改动摘要

### 3.1 perf 热点优化

- `zhttp/include/http_response.h`
- `zhttp/src/http_response.cc`
  - 新增 `HttpResponse::serialize_to(std::string *out, bool include_body)`。
  - 将原先基于 `std::ostringstream` 的响应序列化路径，改为 `std::string` 预估容量后直接 `append()`。
  - 避免了 `std::__ostream_insert`、`basic_stringbuf::overflow` 这一类流式组装热点。

### 3.2 cache / 访存路径优化

- `zhttp/src/http_server.cc`
  - 在连接上下文中缓存 `remote_addr`，避免每个请求都走一次 `IPv4Address::to_string()`。
- `zhttp/include/http_request.h`
- `zhttp/src/http_request.cc`
  - 为请求头维护 `normalized_headers_` 索引，避免热路径上反复线性扫描和重复 `to_lower(pair.first)`。
  - 为 `remote_addr()` 增加延迟 resolver / 显式设置接口。
- `zhttp/src/request_body_middleware.cc`
  - 将 `Content-Type` 读取与 MIME 规范化收敛为每请求一次，避免 `is_json()/is_form_urlencoded()/is_multipart()` 在默认中间件链路里重复查表与重复大小写转换。

### 3.3 回归测试与压测工件

- `zhttp/tests/unit/http_response_test.cc`
  - 新增 `SerializeToMatchesSerializeOutput`，覆盖新序列化 API。
- `zhttp/tests/unit/http_request_test.cc`
  - 新增 `LazyRemoteAddrResolverRunsOnlyOnce`。
  - 新增 `HeaderOverwriteUsesLatestValueCaseInsensitively`。
- `zhttp/tests/benchmark/run_zhttp_wrk_perf.sh`
  - 新增统一压测脚本，支持 `baseline / perf / valgrind` 三种模式。

## 4. 关键 Artifacts

- 基线吞吐：
  - `/home/betty/repositories/zlynx/.worktrees/zhttp-perf-baseline/zhttp/tests/benchmark/perf_results/baseline_20260410_201520/benchmark_output.txt`
- 优化后吞吐复测：
  - `/home/betty/repositories/zlynx/zhttp/tests/benchmark/perf_results/baseline_20260410_204542/benchmark_output.txt`
  - `/home/betty/repositories/zlynx/zhttp/tests/benchmark/perf_results/baseline_20260410_204707/benchmark_output.txt`
- 基线 perf：
  - `/home/betty/repositories/zlynx/.worktrees/zhttp-perf-baseline/zhttp/tests/benchmark/perf_results/perf_20260410_193925/perf_server_report.txt`
- 优化后 perf：
  - `/home/betty/repositories/zlynx/zhttp/tests/benchmark/perf_results/perf_20260410_204510/perf_server_report.txt`
- 基线 cachegrind：
  - `/home/betty/repositories/zlynx/.worktrees/zhttp-perf-baseline/zhttp/tests/benchmark/perf_results/valgrind_20260410_194436/cachegrind_report.txt`
- 优化后 cachegrind：
  - `/home/betty/repositories/zlynx/zhttp/tests/benchmark/perf_results/valgrind_20260410_204325/cachegrind_report.txt`

## 5. perf 结果

### 5.1 热点函数变化

| 热点 | 基线占比 | 优化后占比 | 变化 |
|---|---:|---:|---:|
| `zhttp::HttpResponse::serialize[abi:cxx11](bool) const` / `serialize_to(...)` | 8.12% | 2.47% | -69.6% |
| `znet::IPv4Address::to_string[abi:cxx11]() const` | 4.72% | top 热点中消失 | 明显降温 |
| `tolower` | 2.61% | 1.60% | -38.7% |
| `zhttp::RequestBodyMiddleware::before(...)` | 未进入基线 top 列表 | 0.51% | 热度较低 |

优化后仍然靠前的热点：

- `zco::co_recv(...)`：3.20%
- `zco::co_send(...)`：2.84%
- `zhttp::HttpResponse::serialize_to(...)`：2.47%

结论：

- `perf` 方向的第一类优化是明确生效的。
- 原先最突出的 `HttpResponse::serialize()` 已明显降温，`std::ostringstream` 路径不再是主热点。
- 连接地址格式化 `IPv4Address::to_string()` 不再是 server 侧主要 CPU 成本。
- 当前更主要的剩余成本已经回到 `co_recv/co_send` 这类网络协程收发路径。

## 6. cachegrind 结果

### 6.1 为什么不能只看总量

`cachegrind` 的 server 子进程总计数会受到“本次 valgrind 压测期间实际处理了多少请求”的影响。  
本次基线与优化后两次 valgrind 运行完成的请求数不同：

- 基线：`6096 + 6071 = 12167` 请求
- 优化后：`7390 + 7366 = 14756` 请求

因此报告将同时给出两组口径：

1. 单位请求成本：更适合比较“每处理一个请求要付出多少访存和 miss”。
2. miss rate / hit rate：更适合回答“缓存命中率是否真的提升”。

### 6.2 单位请求成本

| 指标 | 基线 | 优化后 | 变化 |
|---|---:|---:|---:|
| Ir / req | 13,447.97 | 8,659.15 | -35.6% |
| Dr / req | 3,922.80 | 2,406.45 | -38.7% |
| Dw / req | 2,262.67 | 1,439.37 | -36.4% |
| D1 misses / req | 38.21 | 35.01 | -8.4% |
| LL misses / req | 4.66 | 3.94 | -15.5% |
| Branch mispredicts / req | 138.79 | 76.72 | -44.7% |

结论：

- 从“每请求成本”看，这轮 cache/访存优化是有效的。
- 单位请求的指令数、数据读写次数、D1/LL miss 数和分支预测失败数都在下降。
- `remote_addr` 缓存与 `RequestBodyMiddleware` 的单次 MIME 判定，确实压缩了热路径上的无效字符串处理和重复访存。

### 6.3 miss rate / hit rate

| 指标 | 基线 | 优化后 | 变化 |
|---|---:|---:|---:|
| D1 miss rate | 0.618% | 0.910% | +0.292 pct |
| D1 hit rate | 99.382% | 99.090% | -0.292 pct |
| LL miss rate | 0.075% | 0.102% | +0.027 pct |
| LL hit rate | 99.925% | 99.898% | -0.027 pct |
| Branch mispredict rate | 7.159% | 5.831% | -1.328 pct |

这里必须如实说明：

- 按用户要求中的“缓存命中率”口径，本轮并没有把全局 D1 / LL hit rate 做成正向提升。
- 按“单位请求的缓存成本”口径，本轮是有收益的，D1 / LL miss 的每请求数量都下降了。

因此第二类优化的准确结论应是：

- 已经降低了单位请求的缓存与分支成本。
- 但没有把全局缓存命中率一起优化上去，这部分目标未完全达成。

## 7. 宏基准吞吐

### 7.1 基线对照

优化前基线：

| 模式 | Requests/sec | Avg Latency |
|---|---:|---:|
| independent | 128,397.65 | 2.78 ms |
| shared | 129,637.67 | 2.52 ms |

优化后两次串行 baseline 复测：

| 运行 | 模式 | Requests/sec | Avg Latency |
|---|---|---:|---:|
| `baseline_20260410_204542` | independent | 131,201.95 | 2.41 ms |
| `baseline_20260410_204542` | shared | 127,505.54 | 4.52 ms |
| `baseline_20260410_204707` | independent | 123,055.18 | 2.45 ms |
| `baseline_20260410_204707` | shared | 118,011.15 | 2.32 ms |

### 7.2 吞吐结论

- `independent` 模式相对基线出现过一次 `+2.2%` 的正收益，但复测波动较大。
- `shared` 模式复测结果低于基线。
- 同一会话内 run-to-run 噪声明显，因此我不把这轮工作表述为“吞吐稳定提升”。

更稳妥的表述是：

- `perf` 热点优化已经成功，把 CPU 热点从响应拼装/地址格式化上移走了。
- `cachegrind` 显示单位请求成本也下降了。
- 但这些收益在当前机器和本次 session 中，没有稳定转化成可重复的宏基准 QPS 增长。

## 8. 验证结果

执行过的验证命令：

```bash
ctest --test-dir build-zhttp-perf \
  -R '^(http_request_test|http_response_test|http_parser_test|http_parser_detailed_test|router_test|router_detailed_test|request_body_middleware_test)$' \
  --output-on-failure
```

结果：

- 7/7 测试通过。

另外记录一个与本次改动无关的已知情况：

- 基线 worktree 中的 `http_server_test` 在本次工作开始前就存在失败，表现为端口返回 `0`；因此它没有被作为本次性能优化的回归准入条件。

## 9. 结论

本次 `zhttp` 性能优化有三点可以确定：

1. `perf` 定位到的热点优化是有效的，`HttpResponse::serialize()` 从主要 CPU 热点明显降温，`IPv4Address::to_string()` 也退出了主热点列表。
2. `cachegrind` 从“单位请求成本”口径看是正收益，单位请求的 Ir / Dr / Dw / D1 miss / LL miss / branch mispredict 都下降。
3. 按“缓存命中率”口径，本轮没有完成正向提升，D1 / LL hit rate 仍略差于基线。

因此最终结论不是“全面提速”，而是：

- 热点函数已经被有效优化；
- 单位请求的缓存与分支成本已经下降；
- 但 end-to-end 吞吐提升不稳定，且全局缓存命中率未被优化上去。

如果继续做下一轮优化，优先建议看三类方向：

1. 继续压缩 `HttpRequest` / `HttpParser` 的对象重建与哈希表写放大。
2. 针对 `co_recv/co_send` 和 `HttpParser::reset()` 做更细粒度的 per-request 分摊分析。
3. 引入更细的 microbenchmark，把 `header lookup / parser reset / middleware dispatch / response serialize` 单独拆开，避免宏基准噪声掩盖真实收益。
