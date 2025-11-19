#include "zlog.h"
#include <iostream>
void bench(const std::string &loggerName, size_t threadNum, size_t messNum, size_t messLen)
{
    // 1. 获取日志器
    zlog::Logger::ptr logger = zlog::getLogger(loggerName);
    if (logger == nullptr)
    {
        return;
    }
    std::cout << "-------------------------------begin------------------------------" << std::endl;

    std::cout << "\t测试日志：线程" << threadNum << "个 " << "日志数量" << messNum << "条 总大小" << (messNum * messLen) / 1024 << "KB" << std::endl;
    // 2. 构建指定长度的字符串
    std::string msg(messLen - 1, 'A'); // 预留'\n'

    // 3. 构建指定数量的线程
    size_t arvgNum = messNum / threadNum;
    std::vector<std::thread> threads;
    std::vector<double> cost_array(threadNum);
    for (size_t i = 0; i < threadNum; ++i)
    {
        threads.emplace_back([&, i]()
                             {
            // 4. 计时
            auto start = std::chrono::high_resolution_clock::now();

            for(size_t j = 0;j < arvgNum;j++)
            {
                logger->ZLOG_FATAL("{}",msg.c_str());
            }
            auto end = std::chrono::high_resolution_clock::now();

            // 5.统计
            std::chrono::duration<double> cost = end - start;
            cost_array[i] = cost.count();
            std::cout<<"\t线程"<<i<<":"<<"\t输出日志数量"<<arvgNum<<", 耗时："<<cost.count()<<"s"<<std::endl; });
    }

    for (size_t i = 0; i < threadNum; ++i)
    {
        threads[i].join();
    }

    // 7. 计算总时间(并发)
    double maxCost = cost_array[0];
    for (size_t i = 0; i < threadNum; ++i)
    {
        maxCost = std::max(maxCost, cost_array[i]);
    }

    double perMessSec = messNum / maxCost;
    double perSizeSec = messNum * messLen / (maxCost * 1024);

    // 8. 输出
    std::cout << "\t总耗时：" << maxCost << '\n';
    std::cout << "\t每秒输出日志数量：" << perMessSec << '\n';
    std::cout << "\t每秒输出日志大小：" << perSizeSec << "KB" << '\n';
    std::cout << "-------------------------------end------------------------------" << std::endl;
}

void syncBench(size_t threadNum)
{
    std::unique_ptr<zlog::GlobalLoggerBuilder> builder(new zlog::GlobalLoggerBuilder());
    builder->buildLoggerName("sync_logger");
    builder->buildLoggerFormatter("%d{%Y-%m-%d %H:%M:%S} %m%n");
    builder->buildLoggerType(zlog::LoggerType::LOGGER_SYNC);
    builder->buildLoggerSink<zlog::FileSink>("./logfile/sync.log");
    builder->buildLoggerSink<zlog::StdOutSink>();
   //builder->buildLoggerSink<zlog::RollBySizeSink>("./logfile/sync.log", 1 * 1024 * 1024);

    zlog::Logger::ptr logger = builder->build();
    bench("sync_logger", threadNum, 1000000, 100);
}
void asyncBench(size_t threadNum)
{
    std::unique_ptr<zlog::GlobalLoggerBuilder> builder(new zlog::GlobalLoggerBuilder());
    builder->buildLoggerName("async_logger");
    builder->buildLoggerFormatter("%d{%Y-%m-%d %H:%M:%S} %m%n");
    builder->buildLoggerType(zlog::LoggerType::LOGGER_ASYNC);
    builder->buildEnalleUnSafe();
    builder->buildLoggerSink<zlog::FileSink>("./logfile/async.log");

    zlog::Logger::ptr logger = builder->build();

    bench("async_logger", threadNum, 1000000, 100);
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        std::cout << "you should ./bench async or sync threadCount" << std::endl;
        return -1;
    }
    std::string loggerName = argv[1];
    size_t threadNum = std::stoi(argv[2]);
    if (loggerName == "sync")
    {
        syncBench(threadNum);
    }
    else
    {
        asyncBench(threadNum);
    }
    return 0;
}