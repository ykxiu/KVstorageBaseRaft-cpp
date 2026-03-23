//
// Created by swx on 23-6-4.
//
#include "clerk.h"
#include "raftServerRpcUtil.h"
#include "util.h"
#include <chrono>
#include <string>
#include <thread>
#include <vector>

std::string Clerk::Get(std::string key) {
  m_requestId++;
  auto requestId = m_requestId;
  int server = m_recentLeaderId;
  raftKVRpcProctoc::GetArgs args;
  args.set_key(key);
  args.set_clientid(m_clientId);
  args.set_requestid(requestId);

  while (true) {
    raftKVRpcProctoc::GetReply reply;
    bool ok = m_servers[server]->Get(&args, &reply);
    if (!ok || reply.err() == ErrWrongLeader) {
      // 修复：原来裸 continue，在 leader 不可用期间（如 snapshot 导致多轮超时）
      // 会以最高速率轮询所有节点，加剧服务端压力，形成重试风暴。
      // 加 5ms 退避：既不影响正常路径（leader 在线时通常一次成功），
      // 又能在异常期间给集群留出恢复窗口。
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      server = (server + 1) % m_servers.size();
      continue;
    }
    if (reply.err() == ErrNoKey) {
      return "";
    }
    if (reply.err() == OK) {
      m_recentLeaderId = server;
      return reply.value();
    }
  }
  return "";
}

void Clerk::PutAppend(std::string key, std::string value, std::string op) {
  m_requestId++;
  auto requestId = m_requestId;
  auto server = m_recentLeaderId;
  while (true) {
    raftKVRpcProctoc::PutAppendArgs args;
    args.set_key(key);
    args.set_value(value);
    args.set_op(op);
    args.set_clientid(m_clientId);
    args.set_requestid(requestId);
    raftKVRpcProctoc::PutAppendReply reply;
    bool ok = m_servers[server]->PutAppend(&args, &reply);
    if (!ok || reply.err() == ErrWrongLeader) {
      DPrintf("【Clerk::PutAppend】原以为的leader：{%d}请求失败，向新leader{%d}重试  ，操作：{%s}", server, server + 1,
              op.c_str());
      if (!ok) {
        DPrintf("重试原因 ，rpc失敗 ，");
      }
      if (reply.err() == ErrWrongLeader) {
        DPrintf("重試原因：非leader");
      }
      // 同 Get：加 5ms 退避，防止重试风暴
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      server = (server + 1) % m_servers.size();
      continue;
    }
    if (reply.err() == OK) {
      m_recentLeaderId = server;
      return;
    }
  }
}

void Clerk::Put(std::string key, std::string value) { PutAppend(key, value, "Put"); }

void Clerk::Append(std::string key, std::string value) { PutAppend(key, value, "Append"); }

void Clerk::Init(std::string configFileName) {
  MprpcConfig config;
  config.LoadConfigFile(configFileName.c_str());
  std::vector<std::pair<std::string, short>> ipPortVt;
  for (int i = 0; i < INT_MAX - 1; ++i) {
    std::string node = "node" + std::to_string(i);
    std::string nodeIp = config.Load(node + "ip");
    std::string nodePortStr = config.Load(node + "port");
    if (nodeIp.empty()) {
      break;
    }
    ipPortVt.emplace_back(nodeIp, atoi(nodePortStr.c_str()));
  }
  for (const auto& item : ipPortVt) {
    std::string ip = item.first;
    short port = item.second;
    m_servers.push_back(std::make_shared<raftServerRpcUtil>(ip, port));
  }
}

Clerk::Clerk() : m_clientId(Uuid()), m_requestId(0), m_recentLeaderId(0) {}
