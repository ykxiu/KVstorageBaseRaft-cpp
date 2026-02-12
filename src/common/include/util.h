
#ifndef UTIL_H
#define UTIL_H

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/access.hpp>
#include <condition_variable>  // pthread_condition_t
#include <functional>
#include <iostream>
#include <mutex>  // pthread_mutex_t
#include <queue>
#include <random>
#include <sstream>
#include <thread>
#include <atomic>
#include "config.h"

template <class F>
class DeferClass {
 public:
  DeferClass(F&& f) : m_func(std::forward<F>(f)) {}
  DeferClass(const F& f) : m_func(f) {}
  ~DeferClass() { m_func(); }

  DeferClass(const DeferClass& e) = delete;
  DeferClass& operator=(const DeferClass& e) = delete;

 private:
  F m_func;
};

#define _CONCAT(a, b) a##b
#define _MAKE_DEFER_(line) DeferClass _CONCAT(defer_placeholder, line) = [&]()

#undef DEFER
#define DEFER _MAKE_DEFER_(__LINE__)

void DPrintf(const char* format, ...);

void myAssert(bool condition, std::string message = "Assertion failed!");

template <typename... Args>
std::string format(const char* format_str, Args... args) {
    int size_s = std::snprintf(nullptr, 0, format_str, args...) + 1; // "\0"
    if (size_s <= 0) { throw std::runtime_error("Error during formatting."); }
    auto size = static_cast<size_t>(size_s);
    std::vector<char> buf(size);
    std::snprintf(buf.data(), size, format_str, args...);
    return std::string(buf.data(), buf.data() + size - 1);  // remove '\0'
}

std::chrono::_V2::system_clock::time_point now();

std::chrono::milliseconds getRandomizedElectionTimeout();
void sleepNMilliseconds(int N);

// ////////////////////////异步写日志的日志队列
// read is blocking!!! LIKE  go chan
// 无锁 MPSC 队列 (Vyukov MPSC queue)
// - 多个生产者调用 Push
// - 单个消费者调用 Pop / timeOutPop
template <typename T>
class LockQueue {
 public:
  LockQueue() {
    Node* dummy = new Node();
    m_head = dummy;                 // consumer 所有
    m_tail.store(dummy);            // producers 原子交换
  }

  ~LockQueue() {
    // 清理剩余节点
    Node* node = m_head;
    while (node) {
      Node* next = node->next.load(std::memory_order_relaxed);
      delete node;
      node = next;
    }
  }

  // 多个producer并发安全，无锁
  void Push(const T& data) {
    Node* node = new Node(data);
    node->next.store(nullptr, std::memory_order_relaxed);

    Node* prev = std::atomic_exchange_explicit(&m_tail, node, std::memory_order_acq_rel);
    prev->next.store(node, std::memory_order_release);

    // 唤醒consumer（等待时会使用m_waitMutex），此处不保护数据结构，只用于信号
    m_condvariable.notify_one();
  }

  // 单个consumer 调用，阻塞直到有数据
  T Pop() {
    for (;;) {
      T out;
      if (tryPop(out)) return out;

      std::unique_lock<std::mutex> lk(m_waitMutex);
      m_condvariable.wait(lk, [&] { return !isEmpty(); });
      // loop 再次尝试取数据
    }
  }

  // 带超时的Pop，超时返回false
  bool timeOutPop(int timeout, T* ResData) {
    T out;
    if (tryPop(out)) {
      *ResData = std::move(out);
      return true;
    }

    std::unique_lock<std::mutex> lk(m_waitMutex);
    if (!m_condvariable.wait_for(lk, std::chrono::milliseconds(timeout), [&] { return !isEmpty(); })) {
      return false;  // 超时且仍然为空
    }

    if (tryPop(out)) {
      *ResData = std::move(out);
      return true;
    }
    return false;
  }

 private:
  struct Node {
    std::atomic<Node*> next;
    T data;
    bool hasData;

    Node() : next(nullptr), data(), hasData(false) {}
    Node(const T& d) : next(nullptr), data(d), hasData(true) {}
  };

  // tryPop: 非阻塞尝试弹出
  bool tryPop(T& out) {
    Node* head = m_head;
    Node* next = head->next.load(std::memory_order_acquire);
    if (next == nullptr) return false;

    // next 存在，消费它
    out = std::move(next->data);
    m_head = next;
    delete head;  // 只有consumer删除节点，安全
    return true;
  }

  bool isEmpty() {
    Node* head = m_head;
    Node* next = head->next.load(std::memory_order_acquire);
    return next == nullptr;
  }

  // consumer 所有权（非原子）
  Node* m_head;
  // producers 原子尾指针
  std::atomic<Node*> m_tail;

  // 仅用于在Pop上等待的同步；数据结构本身依然为无锁
  std::mutex m_waitMutex;
  std::condition_variable m_condvariable;
};
// 两个对锁的管理用到了RAII的思想，防止中途出现问题而导致资源无法释放的问题！！！
// std::lock_guard 和 std::unique_lock 都是 C++11 中用来管理互斥锁的工具类，它们都封装了 RAII（Resource Acquisition Is
// Initialization）技术，使得互斥锁在需要时自动加锁，在不需要时自动解锁，从而避免了很多手动加锁和解锁的繁琐操作。
// std::lock_guard 是一个模板类，它的模板参数是一个互斥量类型。当创建一个 std::lock_guard
// 对象时，它会自动地对传入的互斥量进行加锁操作，并在该对象被销毁时对互斥量进行自动解锁操作。std::lock_guard
// 不能手动释放锁，因为其所提供的锁的生命周期与其绑定对象的生命周期一致。 std::unique_lock
// 也是一个模板类，同样的，其模板参数也是互斥量类型。不同的是，std::unique_lock 提供了更灵活的锁管理功能。可以通过
// lock()、unlock()、try_lock() 等方法手动控制锁的状态。当然，std::unique_lock 也支持 RAII
// 技术，即在对象被销毁时会自动解锁。另外， std::unique_lock 还支持超时等待和可中断等待的操作。

// 这个Op是kv传递给raft的command
class Op {
 public:
  // Your definitions here.
  // Field names must start with capital letters,
  // otherwise RPC will break.
  std::string Operation;  // "Get" "Put" "Append"
  std::string Key;
  std::string Value;
  std::string ClientId;  //客户端号码
  int RequestId;         //客户端号码请求的Request的序列号，为了保证线性一致性
                         // IfDuplicate bool // Duplicate command can't be applied twice , but only for PUT and APPEND

 public:
  // todo
  //为了协调raftRPC中的command只设置成了string,这个的限制就是正常字符中不能包含|
  //当然后期可以换成更高级的序列化方法，比如protobuf
  std::string asString() const {
    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);

    // write class instance to archive
    oa << *this;
    // close archive

    return ss.str();
  }

  bool parseFromString(std::string str) {
    std::stringstream iss(str);
    boost::archive::text_iarchive ia(iss);
    // read class state from archive
    ia >> *this;
    return true;  // todo : 解析失敗如何處理，要看一下boost庫了
  }

 public:
  friend std::ostream& operator<<(std::ostream& os, const Op& obj) {
    os << "[MyClass:Operation{" + obj.Operation + "},Key{" + obj.Key + "},Value{" + obj.Value + "},ClientId{" +
              obj.ClientId + "},RequestId{" + std::to_string(obj.RequestId) + "}";  // 在这里实现自定义的输出格式
    return os;
  }

 private:
  friend class boost::serialization::access;
  template <class Archive>
  void serialize(Archive& ar, const unsigned int version) {
    ar& Operation;
    ar& Key;
    ar& Value;
    ar& ClientId;
    ar& RequestId;
  }
};

///////////////////////////////////////////////kvserver reply err to clerk

const std::string OK = "OK";
const std::string ErrNoKey = "ErrNoKey";
const std::string ErrWrongLeader = "ErrWrongLeader";

////////////////////////////////////获取可用端口

bool isReleasePort(unsigned short usPort);

bool getReleasePort(short& port);

// int main(int argc, char** argv)
//{
//     short port = 9060;
//     if(getReleasePort(port)) //在port的基础上获取一个可用的port
//     {
//         std::cout << "可用的端口号为：" << port << std::endl;
//     }
//     else
//     {
//         std::cout << "获取可用端口号失败！" << std::endl;
//     }
//     return 0;
// }

#endif  //  UTIL_H