#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <csignal>
#include <gperftools/profiler.h>   // ← 新增

#include "clerk.h"

using namespace std;
using Clock = std::chrono::steady_clock;
using us_t = long long;  // microseconds

static const int WRITE_CLIENTS = 5;    // 纯写线程数
static const int READ_CLIENTS = 5;     // 纯读线程数
static const int TEST_DURATION = 10;   // 测试时长（秒）

// -------- 延迟采集 --------
std::mutex g_latMtx;
std::vector<us_t> g_write_lat;  // Put 延迟（微秒）
std::vector<us_t> g_read_lat;   // Get 延迟（微秒）

// -------- 操作计数 --------
std::atomic<long> g_write_ops{0};
std::atomic<long> g_read_ops{0};

// 计算百分位（要求 v 已排序）
static us_t percentile(const std::vector<us_t>& v, double p) {
  if (v.empty()) return 0;
  size_t idx = static_cast<size_t>(v.size() * p / 100.0);
  if (idx >= v.size()) idx = v.size() - 1;
  return v[idx];
}

// -------- 写线程（纯 Put）--------
void write_worker(int id) {
  Clerk client;
  client.Init("test.conf");

  std::vector<us_t> lats;
  lats.reserve(4096);

  int seq = 0;
  auto deadline = Clock::now() + std::chrono::seconds(TEST_DURATION);

  while (Clock::now() < deadline) {
    std::string key = "w" + std::to_string(id);
    std::string val = std::to_string(seq++);

    auto t0 = Clock::now();
    client.Put(key, val);
    auto t1 = Clock::now();

    lats.push_back(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    ++g_write_ops;
  }

  std::lock_guard<std::mutex> lk(g_latMtx);
  g_write_lat.insert(g_write_lat.end(), lats.begin(), lats.end());
}

// -------- 读线程（纯 Get）--------
void read_worker(int id) {
  Clerk client;
  client.Init("test.conf");

  std::vector<us_t> lats;
  lats.reserve(4096);

  // 读自己写线程的 key，保证 key 存在
  std::string key = "w" + std::to_string(id % WRITE_CLIENTS);

  auto deadline = Clock::now() + std::chrono::seconds(TEST_DURATION);

  while (Clock::now() < deadline) {
    auto t0 = Clock::now();
    client.Get(key);
    auto t1 = Clock::now();

    lats.push_back(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    ++g_read_ops;
  }

  std::lock_guard<std::mutex> lk(g_latMtx);
  g_read_lat.insert(g_read_lat.end(), lats.begin(), lats.end());
}

// -------- 打印延迟分布 --------
static void print_stats(const char* label, std::vector<us_t>& lats, long ops, double elapsed) {
  std::sort(lats.begin(), lats.end());
  double qps = ops / elapsed;
  std::cout << "  " << label << ":\n";
  std::cout << "    ops=" << ops << "  QPS=" << (long)qps << "\n";
  std::cout << "    P50=" << percentile(lats, 50) << " us"
            << "  P95=" << percentile(lats, 95) << " us"
            << "  P99=" << percentile(lats, 99) << " us"
            << "  max=" << (lats.empty() ? 0 : lats.back()) << " us\n";
}

int main() {
  signal(SIGPIPE, SIG_IGN);       // ← 新增
  ProfilerStart("client.prof");   // ← 新增：启动 profiler

  std::cout << "=== Raft KV Benchmark ===\n";
  std::cout << "Write clients : " << WRITE_CLIENTS << "\n";
  std::cout << "Read  clients : " << READ_CLIENTS << "\n";
  std::cout << "Duration      : " << TEST_DURATION << " s\n\n";

  auto t_start = Clock::now();

  std::vector<std::thread> threads;
  threads.reserve(WRITE_CLIENTS + READ_CLIENTS);

  for (int i = 0; i < WRITE_CLIENTS; ++i) threads.emplace_back(write_worker, i);
  for (int i = 0; i < READ_CLIENTS; ++i) threads.emplace_back(read_worker, i);

  for (auto& t : threads) t.join();

  double elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_start).count() / 1000.0;

  long total = g_write_ops.load() + g_read_ops.load();

  std::cout << "====================================\n";
  std::cout << "Elapsed : " << elapsed << " s\n";
  std::cout << "Total QPS : " << (long)(total / elapsed) << "\n\n";
  print_stats("Put (write)", g_write_lat, g_write_ops.load(), elapsed);
  print_stats("Get (read) ", g_read_lat,  g_read_ops.load(),  elapsed);
  std::cout << "====================================\n";

  ProfilerStop();  // ← 新增：停止 profiler
  return 0;
}
