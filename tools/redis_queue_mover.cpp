/**
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: eyjian@qq.com or eyjian@gmail.com
 *
 * 搬移redis队列工具，用于将redis一个队列的数据复制到另一队列中，要求为左进右出
 */
#include <r3c/r3c.h>
#include <mooon/sys/atomic.h>
#include <mooon/sys/safe_logger.h>
#include <mooon/sys/signal_handler.h>
#include <mooon/sys/thread_engine.h>
#include <mooon/sys/utils.h>
#include <mooon/utils/args_parser.h>
#include <mooon/utils/scoped_ptr.h>
#include <mooon/utils/string_utils.h>

// 如何确定一个redis的key？
// 通过前缀prefix加序号的方式，其它前缀prefix由参数指定，但可以为空，
// 序号从0开始按1递增，最大值为参数queues的值减1
//
// 假设前缀prefix值为“mooon:”，queues值为3，则有如下三个key：
// mooon:0
// mooon:1
// mooon:2

// 队列数
// 源队列和目标队列数均由此参数决定，也即源队列和目标队列的个数是相等的
INTEGER_ARG_DEFINE(int, queues, 1, 1, 2019, "Number of queues, e.g. --queues=1");

// 线程数系数，
// 注意并不是线程数，线程数为：threads * queues，
// 假设queues参数值为2，threads参数值为3，则线程数为6
INTEGER_ARG_DEFINE(int, threads, 1, 1, 20, "(threads * queues) to get number of move threads, e.g., --threads=1");

// 源redis
STRING_ARG_DEFINE(src_redis, "", "Nodes of source redis, e.g., --src_redis=127.0.0.1:6379,127.0.0.1:6380");

// 目标redis
// 当源和目标相同时，应当指定不同的prefix，虽然也可以都相同，但那样无实际意义了
// 如果不指定目标 redis，则落本地文件。
STRING_ARG_DEFINE(dst_redis, "", "Nodes of destination redis, e.g., --dst_redis=127.0.0.1:6381,127.0.0.1:6382");

// 目标file
// dst_redis和dst_file不能同时为空，
// 且dst_redis优先，即如果设置了dst_redis，就会忽略dst_file。
STRING_ARG_DEFINE(dst_file, "", "File to store, e.g., --dst_file=/home/mooon/mooon.data");

// 源队列Key前缀
STRING_ARG_DEFINE(src_prefix, "", "Key prefix of source queue, e.g., --src_prefix='mooon:'");

// 目标队列Key前缀
STRING_ARG_DEFINE(dst_prefix, "", "Key prefix of destination queue, e.g., --src_prefix='mooon:'");

// 源队列Key是否仅由前缀组成，即src_prefix是key，或只是key的前缀
INTEGER_ARG_DEFINE(int, src_only_prefix, 0, 0, 1, "Prefix is the key of source");

// 目标队列Key是否仅由前缀组成，即dst_prefix是key，或只是key的前缀
INTEGER_ARG_DEFINE(int, dst_only_prefix, 0, 0, 1, "Prefix is the key of destination");

// 源 redis 读写超时时长（单位：毫秒）
INTEGER_ARG_DEFINE(int, src_timeout, 10000, 1, 3600, "Source redis timeout in seconds");

// 目标 redis 读写超时时长（单位：毫秒）
INTEGER_ARG_DEFINE(int, dst_timeout, 10000, 1, 3600, "Destination redis timeout in seconds");

// 源 redis 连接密码
STRING_ARG_DEFINE(src_password, "", "Password for source redis");

// 目标 redis 连接密码
STRING_ARG_DEFINE(dst_password, "", "Password for destination redis");

// 多少个时输出一次计数
INTEGER_ARG_DEFINE(int, tick, 10000, 1, 10000000, "Times to tick");

// 统计频率（单位：秒）
INTEGER_ARG_DEFINE(int, stat_interval, 2, 1, 86400, "Interval to stat in seconds");

// 轮询队列和重试操作的间隔（单位为毫秒）
INTEGER_ARG_DEFINE(int, retry_interval, 100, 1, 1000000, "Interval in milliseconds to poll or retry");

// 批量数，即一次批量移动多少
INTEGER_ARG_DEFINE(int, batch, 1, 1, 100000, "Batch to move");

// label
// 可选的，
// 用来区分不同的 redis_queue_mover 进程，
// 以方便监控识别
STRING_ARG_DEFINE(label, "", "Used to distinguish between different processes, e.g., --label='test'");

static mooon::sys::CAtomic<bool> g_stop(false);
#if __WORDSIZE==64
static atomic8_t g_num_moved;
#else
static atomic_t g_num_moved;
#endif

static void on_terminated();
static void signal_thread_proc(); // 信号线程
static void stat_thread_proc(); // 统计线程
static void move_thread_proc(int i); // 移动线程
static std::string get_src_key(int i);
static std::string get_dst_key(int i);

int main(int argc, char* argv[])
{
    std::string errmsg;

    if (!mooon::utils::parse_arguments(argc, argv, &errmsg))
    {
        fprintf(stderr, "%s.\n\n", errmsg.c_str());
        fprintf(stderr, "%s\n", mooon::utils::g_help_string.c_str());
        exit(1);
    }
    else if (mooon::argument::src_redis->value().empty())
    {
        fprintf(stderr, "Parameter[--src_redis] is not set.\n\n");
        fprintf(stderr, "%s\n", mooon::utils::g_help_string.c_str());
        exit(1);
    }
    else if (mooon::argument::dst_redis->value().empty() &&
             mooon::argument::dst_file->value().empty())
    {
        fprintf(stderr, "Both parameter[--dst_redis] and parameter[--dst_file] are not set.\n\n");
        fprintf(stderr, "%s\n", mooon::utils::g_help_string.c_str());
        exit(1);
    }
    else if (mooon::argument::src_prefix->value().empty())
    {
        fprintf(stderr, "Parameter[--src_prefix] is not set.\n\n");
        fprintf(stderr, "%s\n", mooon::utils::g_help_string.c_str());
        exit(1);
    }
    else if (!mooon::argument::dst_redis->value().empty() &&
              mooon::argument::dst_prefix->value().empty())
    {
        fprintf(stderr, "Parameter[--dst_prefix] is not set.\n\n");
        fprintf(stderr, "%s\n", mooon::utils::g_help_string.c_str());
        exit(1);
    }

    try
    {
#if __WORDSIZE==64
        atomic8_set(&g_num_moved, 0);
#else
        atomic_set(&g_num_moved, 0);
#endif

        if (mooon::argument::label->value().empty())
            mooon::sys::g_logger = mooon::sys::create_safe_logger();
        else
            mooon::sys::g_logger = mooon::sys::create_safe_logger(true, mooon::SIZE_8K, mooon::argument::label->value());
        MYLOG_INFO("Source redis: %s.\n", mooon::argument::src_redis->c_value());
        MYLOG_INFO("Destination redis: %s.\n", mooon::argument::dst_redis->c_value());
        MYLOG_INFO("Source key prefix: %s.\n", mooon::argument::src_prefix->c_value());
        MYLOG_INFO("Destination key prefix: %s.\n", mooon::argument::dst_prefix->c_value());
        MYLOG_INFO("Number of queues: %d.\n", mooon::argument::queues->value());
        MYLOG_INFO("Factor of threads: %d.\n", mooon::argument::threads->value());
        MYLOG_INFO("Number of threads: %d.\n", mooon::argument::threads->value() * mooon::argument::queues->value());
        MYLOG_INFO("Number of batch to move: %d.\n", mooon::argument::batch->value());
        MYLOG_INFO("Only prefix of source: %d.\n", mooon::argument::src_only_prefix->value());
        MYLOG_INFO("Only prefix of destination: %d.\n", mooon::argument::dst_only_prefix->value());

        mooon::sys::CThreadEngine* signal_thread = new mooon::sys::CThreadEngine(mooon::sys::bind(&signal_thread_proc));
        mooon::sys::CThreadEngine* stat_thread = new mooon::sys::CThreadEngine(mooon::sys::bind(&stat_thread_proc));

        const int num_queues = mooon::argument::queues->value();
        const int num_threads = num_queues * mooon::argument::threads->value();
        mooon::sys::CThreadEngine** thread_engines = new mooon::sys::CThreadEngine*[num_threads];
        for (int i=0; i<num_threads; ++i)
        {
            thread_engines[i] = new mooon::sys::CThreadEngine(mooon::sys::bind(&move_thread_proc, i));
        }
        for (int i=0; i<num_threads; ++i)
        {
            thread_engines[i]->join();
            delete thread_engines[i];
        }
        delete []thread_engines;

        stat_thread->join();
        delete stat_thread;
        signal_thread->join();
        delete signal_thread;
        MYLOG_INFO("RedisQueueMover process exit now.\n");
        return 0;
    }
    catch (r3c::CRedisException& ex)
    {
        MYLOG_ERROR("%s.\n", ex.str().c_str());
        exit(1);
    }
    catch (mooon::sys::CSyscallException& ex)
    {
        MYLOG_ERROR("%s.\n", ex.str().c_str());
        exit(1);
    }
}

void on_terminated()
{
    g_stop = true;
}

void signal_thread_proc()
{
    while (!g_stop)
    {
        mooon::sys::CSignalHandler::handle(&on_terminated, NULL, NULL, NULL);
    }
}

void stat_thread_proc()
{
    try
    {
        const int seconds = mooon::argument::stat_interval->value();
        uint64_t old_num_moved = 0;
        uint64_t last_num_moved = 0;
        mooon::sys::ILogger* stat_logger;
        if (mooon::argument::label->value().empty())
            stat_logger = mooon::sys::create_safe_logger(true, mooon::SIZE_64, "stat");
        else
            stat_logger = mooon::sys::create_safe_logger(
                    true, mooon::SIZE_32, mooon::utils::CStringUtils::format_string("%s_stat", mooon::argument::label->c_value()));

        stat_logger->enable_raw_log(true, true);
        while (!g_stop)
        {
            mooon::sys::CUtils::millisleep(seconds * 1000);
#if __WORDSIZE==64
            last_num_moved = atomic8_read(&g_num_moved);
#else
            last_num_moved = atomic_read(&g_num_moved);
#endif

            if (last_num_moved > old_num_moved)
            {
                const int num_moved = static_cast<int>((last_num_moved - old_num_moved) / seconds);
                stat_logger->log_raw(" %" PRId64" %" PRId64" %" PRId64" %d/s MOVED\n",
                        last_num_moved, old_num_moved, last_num_moved - old_num_moved, num_moved);
            }

            old_num_moved = last_num_moved;
        }
    }
    catch (mooon::sys::CSyscallException& ex)
    {
        MYLOG_ERROR("Created stat-logger failed: %s.\n", ex.str().c_str());
    }
}

void move_thread_proc(int i)
{
    const int batch = mooon::argument::batch->value();
    const int num_queues = mooon::argument::queues->value();
    const int retry_interval = mooon::argument::retry_interval->value();
    const std::string& src_key = get_src_key(i % num_queues); // 源key
    const std::string& dst_key = get_dst_key(i % num_queues); // 目标key
    r3c::CRedisClient src_redis(
            mooon::argument::src_redis->value(),
            mooon::argument::src_timeout->value(), mooon::argument::src_timeout->value(),
            mooon::argument::src_password->value());
    mooon::utils::ScopedPtr<r3c::CRedisClient> dst_redis;
    int dst_fd = -1;
    uint32_t num_moved = 0; // 已移动的数目
    uint32_t old_num_moved = 0; // 上一次移动的数目
    std::vector<std::string> values;

    try
    {
        if (!mooon::argument::dst_redis->value().empty()) {
            dst_redis.reset(new r3c::CRedisClient (
                    mooon::argument::dst_redis->value(),
                    mooon::argument::dst_timeout->value(), mooon::argument::dst_timeout->value(),
                    mooon::argument::dst_password->value()));
        }
        else {
            dst_fd = open(mooon::argument::dst_file->c_value(), O_RDONLY);
            if (dst_fd == -1)
                THROW_SYSCALL_EXCEPTION(strerror(errno), errno, "open");
        }
        MYLOG_INFO("[%s] => [%s].\n", src_key.c_str(), dst_key.c_str());
    }
    catch (mooon::sys::CSyscallException& ex)
    {
        MYLOG_ERROR("%s.\n", ex.str().c_str());
        return;
    }
    catch (r3c::CRedisException& ex)
    {
        MYLOG_ERROR("%s.\n", ex.str().c_str());
        if (dst_fd != -1)
            close(dst_fd);
        return;
    }
    while (!g_stop)
    {
        values.clear();

        for (int k=0; !g_stop&&k<batch; ++k)
        {
            try
            {
                std::string value;

                if (!src_redis.rpop(src_key, &value))
                {
                    break;
                }
                else
                {
                    if (!mooon::argument::dst_redis->value().empty()) {
                        values.push_back(value);
                    }
                    else {
                        value.append("\n");
                        if (write(dst_fd, value.data(), value.size()) == -1) {
                            MYLOG_ERROR("Writed file://%s error://%s: %s.\n", mooon::argument::dst_file->c_value(), strerror(errno), value.c_str());
                            g_stop = false;
                            break;
                        }
                    }
                    MYLOG_DEBUG("[%d] %s.\n", k, value.c_str());
                }
            }
            catch (r3c::CRedisException& ex)
            {
                MYLOG_ERROR("[%s]: %s.\n", src_key.c_str(), ex.str().c_str());
            }
        } // for
        if (values.empty())
        {
            mooon::sys::CUtils::millisleep(retry_interval);
            continue;
        }

        while (!values.empty())
        {
            try
            {
                if (dst_redis != NULL)
                    dst_redis->lpush(dst_key, values);

                const uint32_t num_moved_ = static_cast<uint32_t>(values.size());
#if __WORDSIZE==64
                atomic8_add(num_moved_, &g_num_moved);
#else
                atomic_add(num_moved_, &g_num_moved);
#endif

                num_moved += num_moved_;
                if (num_moved - old_num_moved >= static_cast<uint32_t>(mooon::argument::tick->value()))
                {
                    old_num_moved = num_moved;
                    MYLOG_INFO("[%s]=>[%s]: %u.\n", src_key.c_str(), dst_key.c_str(), num_moved);
                }
                break;
            }
            catch (r3c::CRedisException& ex)
            {
                MYLOG_ERROR("[%s]=>[%s]: %s.\n", src_key.c_str(), dst_key.c_str(), ex.str().c_str());
                mooon::sys::CUtils::millisleep(retry_interval);
            }
        } // while (!values.empty())
    } // while (!g_stop)

    close(dst_fd);
    MYLOG_INFO("RedisQueueMover thread %d exit now.\n", i);
}

std::string get_src_key(int i)
{
    if (1 == mooon::argument::src_only_prefix->value())
        return mooon::argument::src_prefix->value();
    else
        return mooon::utils::CStringUtils::format_string("%s%d", mooon::argument::src_prefix->c_value(), (int)i);
}

std::string get_dst_key(int i)
{
    if (1 == mooon::argument::dst_only_prefix->value())
        return mooon::argument::dst_prefix->value();
    else
        return mooon::utils::CStringUtils::format_string("%s%d", mooon::argument::dst_prefix->c_value(), (int)i);
}
