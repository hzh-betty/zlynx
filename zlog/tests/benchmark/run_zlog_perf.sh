#!/bin/bash
# zlog性能分析脚本 - 支持同步与异步模式对比测试
# 使用perf工具采集CPU性能数据和缓存命中率

set -e

# 默认参数
MODE=${1:-"both"}      # sync/async/both
THREADS=${2:-4}
COUNT=${3:-1000000}
MSG_SIZE=${4:-128}

show_help() {
    echo "用法: $0 [mode] [threads] [count] [msg_size]"
    echo ""
    echo "参数:"
    echo "  mode      测试模式: sync/async/both (默认: both)"
    echo "  threads   线程数 (默认: 4)"
    echo "  count     日志条数 (默认: 1000000)"
    echo "  msg_size  消息大小(字节) (默认: 128)"
    echo ""
    echo "示例:"
    echo "  $0 sync 4 500000 64     # 同步模式，4线程，50万条，64字节"
    echo "  $0 async 8 1000000 256  # 异步模式，8线程，100万条，256字节"
    echo "  $0 both                  # 对比测试两种模式"
    exit 0
}

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    show_help
fi

# 运行单次perf测试
run_perf_test() {
    local test_mode=$1
    local prefix=$2
    
    # 创建输出目录
    local output_dir="perf_results/zlog_${prefix}_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$output_dir"
    echo "输出目录: $output_dir"
    
    echo ""
    echo "=========================================="
    if [ "$test_mode" = "sync" ]; then
        echo "性能分析: 同步日志模式"
    else
        echo "性能分析: 异步日志模式"
    fi
    echo "线程数: $THREADS, 日志条数: $COUNT, 消息大小: ${MSG_SIZE}字节"
    echo "=========================================="
    
    # 构建启动命令
    local cmd="./zlog_perf_bench -m $test_mode -t $THREADS -c $COUNT -s $MSG_SIZE -o ${output_dir}/logs"
    
    echo "[1] 使用perf record记录CPU性能数据..."
    echo "命令: $cmd"
    perf record -F 99 -g -o ${output_dir}/perf_cpu.data -- $cmd > ${output_dir}/bench.log 2>&1 &
    PERF_PID=$!
    
    # 等待perf启动
    sleep 0.5
    
    # 获取实际的benchmark进程PID
    BENCH_PID=$(pgrep -P $PERF_PID 2>/dev/null | head -1)
    if [ -z "$BENCH_PID" ]; then
        # 如果找不到子进程，用perf进程本身
        BENCH_PID=$PERF_PID
    fi
    echo "Perf PID: $PERF_PID, Benchmark PID: $BENCH_PID"
    
    echo "[2] 使用perf stat记录缓存命中率..."
    perf stat -e cache-references,cache-misses,instructions,cycles,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses -p $BENCH_PID 2>${output_dir}/perf_stat.txt &
    STAT_PID=$!
    
    # 等待测试完成
    echo "[3] 等待测试完成..."
    wait $PERF_PID 2>${output_dir}/perf_record.err || true
    wait $STAT_PID 2>/dev/null || true
    
    echo ""
    echo "=========================================="
    echo "性能分析报告 - $prefix"
    echo "=========================================="
    
    echo ""
    echo "=== Benchmark结果 ==="
    cat ${output_dir}/bench.log
    
    echo ""
    echo "=== Perf Stat缓存统计 ==="
    cat ${output_dir}/perf_stat.txt 2>/dev/null || echo "perf stat数据收集失败"
    
    echo ""
    echo "=== 生成perf性能数据 ==="
    if [ -f ${output_dir}/perf_cpu.data ]; then
        # 生成函数性能报告
        perf report -i ${output_dir}/perf_cpu.data -n --stdio --sort symbol,dso 2>/dev/null > ${output_dir}/perf_functions.txt || echo "perf report生成失败"
        echo "函数性能数据 (前50行):"
        head -50 ${output_dir}/perf_functions.txt
    else
        echo "perf_cpu.data文件不存在"
    fi
    
    echo ""
    echo "=========================================="
    echo "分析文件已生成到目录: $output_dir"
    echo "=========================================="
    echo "数据文件:"
    echo "  - perf_cpu.data: CPU性能数据"
    echo "  - perf_stat.txt: 缓存统计"
    echo ""
    echo "报告文件:"
    echo "  - perf_functions.txt: 函数性能数据"
    echo "  - bench.log: Benchmark日志"
    echo "  - logs/: 日志输出文件"
    echo "=========================================="
}

# 提取关键指标用于对比
extract_metrics() {
    local dir=$1
    local name=$2
    
    echo "--- $name ---"
    
    # 从bench.log提取吞吐量和延迟
    if [ -f "${dir}/bench.log" ]; then
        grep -E "Throughput:|Latency" ${dir}/bench.log || true
    fi
    
    # 从perf_stat.txt提取缓存命中率
    if [ -f "${dir}/perf_stat.txt" ]; then
        echo "缓存统计:"
        grep -E "cache-misses|L1-dcache-load-misses|LLC-load-misses" ${dir}/perf_stat.txt | head -3 || true
    fi
    
    echo ""
}

# 主流程
case $MODE in
    "sync")
        run_perf_test "sync" "sync"
        ;;
    "async")
        run_perf_test "async" "async"
        ;;
    "both")
        echo "=========================================="
        echo "开始同步与异步日志性能对比测试"
        echo "=========================================="
        
        run_perf_test "sync" "sync"
        
        echo ""
        echo "等待3秒后开始异步测试..."
        sleep 3
        
        run_perf_test "async" "async"
        
        echo ""
        echo "=========================================="
        echo "性能对比总结"
        echo "=========================================="
        echo ""
        
        # 找到最新的测试结果目录
        SYNC_DIR=$(find perf_results -maxdepth 1 -name "zlog_sync_*" -type d 2>/dev/null | sort | tail -1)
        ASYNC_DIR=$(find perf_results -maxdepth 1 -name "zlog_async_*" -type d 2>/dev/null | sort | tail -1)
        
        if [ -n "$SYNC_DIR" ]; then
            extract_metrics "$SYNC_DIR" "同步模式"
        fi
        
        if [ -n "$ASYNC_DIR" ]; then
            extract_metrics "$ASYNC_DIR" "异步模式"
        fi
        
        echo "=========================================="
        echo "对比测试完成"
        echo "=========================================="
        ;;
    *)
        echo "错误: 未知模式 '$MODE'"
        show_help
        ;;
esac
