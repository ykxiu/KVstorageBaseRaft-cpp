# CLAUDE.md — KVstorageBaseRaft-cpp

## 项目概述

基于 Raft 共识算法的分布式 KV 存储系统，使用 C++20 实现。

**三层架构：**
```
Client (Clerk) → KV Server (RPC) → Raft Engine → Skip List Storage
```

## 构建

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
# 产物：bin/raftCoreRun（服务端）、bin/callerMain（客户端基准）
```

**要求：** CMake 3.22+、Muduo、Protobuf、Boost

## 运行示例集群

```bash
cd example/raftCoreExample
# 生成配置文件（test.conf），定义各节点地址
# 启动 N 个节点（各占一个终端）
./raftCoreRun <节点数量> <当前节点ID>
# 运行客户端
./callerMain
```

持久化文件：`raftstatePersist{id}.txt`、`snapshotPersist{id}.txt`

## 关键源文件

| 路径 | 说明 |
|------|------|
| `src/raftCore/raft.cpp` | Raft 完整实现（选举、日志复制、快照），1020 行 |
| `src/raftCore/include/raft.h` | Raft 类定义 |
| `src/raftCore/kvServer.cpp` | KV 状态机（Get/Put/Append + 去重） |
| `src/raftCore/include/kvServer.h` | KV 服务器头文件 |
| `src/raftClerk/clerk.cpp` | 客户端（自动重试 + leader 发现） |
| `src/rpc/rpcprovider.cpp` | Muduo-based RPC 服务端（64 线程池） |
| `src/rpc/mprpcchannel.cpp` | RPC 客户端通道 |
| `src/skipList/include/skipList.h` | 模板跳表（O(log n)，线程安全） |
| `src/common/include/config.h` | 所有时序常量 |
| `src/common/include/util.h` | ThreadPool、LockQueue、DEFER 宏 |
| `src/raftRpcPro/raftRPC.proto` | Raft RPC 消息定义 |
| `src/raftRpcPro/kvServerRPC.proto` | KV RPC 消息定义 |

## 代码规范

- **代码风格：** Google C++ 风格（`.clang-format` 强制执行）
- **成员变量：** `m_` 前缀（如 `m_logs`、`m_currentTerm`、`m_commitIndex`）
- **函数命名：** camelCase（如 `doElection()`、`appendEntries()`）
- **Protobuf 字段：** PascalCase（如 `CurrentTerm`、`VoteGranted`）
- **格式化：** 提交前运行 `./format.sh`

## 并发模式

- 全局互斥锁 `m_mtx` 保护所有 Raft 可变状态
- 条件变量：`m_applyCv`（触发 apply）、`m_replicateCv`（触发复制）
- DEFER 宏用于 RAII 延迟执行（如 `DEFER { persist(); };`）
- 修改 Raft 状态后必须调用 `persist()` 持久化
- 对 protobuf 快照解析使用 Boost 异常处理

## 时序常量（`src/common/include/config.h`）

| 常量 | 默认值 | 说明 |
|------|--------|------|
| `HeartBeatTimeout` | 25ms | 心跳间隔 |
| Election timeout | 300–500ms | 随机化选举超时 |
| `CONSENSUS_TIMEOUT` | 500ms | 客户端共识等待超时 |
| `debugMul` | 1 | 调试时可放大所有超时 |

## 调试

- `DPrintf()`：条件调试日志，由 `src/common/util.cpp` 中的 `Debug` 标志控制
- `myAssert()`：致命断言（关键路径中已替换为 `DPrintf() + return`，避免进程崩溃）
- 启用调试：将 `util.cpp` 中的 `Debug` 设为 `true`

## 已知架构决策

- **写操作：** Start() 后立即触发复制（延迟 ~1 RTT，而非等待心跳）
- **读操作：** ReadIndex 机制（本地读取，延迟 <1ms，非日志复制）
- **Apply：** 事件驱动（条件变量），非轮询
- **心跳：** 复用线程池，避免频繁创建线程（每秒 160+ 次）
- **去重：** 每个客户端维护 `clientId + requestId`，确保 exactly-once 语义

## 添加新 RPC 方法

1. 在对应 `.proto` 文件中定义消息和服务
2. 重新生成：`protoc --cpp_out=. *.proto`
3. 在 `kvServer` 中实现处理器
4. 在 `clerk.cpp` 中添加客户端调用（含重试逻辑）
