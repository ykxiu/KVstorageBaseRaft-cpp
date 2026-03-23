//
// Created by swx on 23-12-28.
//
#include <iostream>
#include <fstream>
#include <string>
#include <random>
#include <unistd.h>
#include "raft.h"
#include <kvServer.h>

void ShowArgsHelp();

// 轮询 configFileName，直到里面已包含 expectedNodes 个节点的 ip 条目，最多等待 maxWaitSec 秒
static bool waitForAllNodesReady(const std::string &configFileName, int expectedNodes, int maxWaitSec = 30) {
  for (int waited = 0; waited < maxWaitSec * 10; ++waited) {
    std::ifstream f(configFileName);
    int count = 0;
    std::string line;
    while (std::getline(f, line)) {
      // 每个节点写一行 "node<i>ip=..."
      if (line.find("ip=") != std::string::npos) {
        ++count;
      }
    }
    if (count >= expectedNodes) {
      return true;
    }
    usleep(100 * 1000);  // 100ms
  }
  return false;
}

int main(int argc, char **argv) {
  //////////////////////////////////读取命令参数：节点数量、写入raft节点节点信息到哪个文件
  if (argc < 2) {
    ShowArgsHelp();
    exit(EXIT_FAILURE);
  }
  int c = 0;
  int nodeNum = 0;
  std::string configFileName;
  // 默认随机端口；可用 -p 指定固定起始端口，方便测试脚本复用 test.conf
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(10000, 29999);
  unsigned short startPort = static_cast<unsigned short>(dis(gen));

  while ((c = getopt(argc, argv, "n:f:p:")) != -1) {
    switch (c) {
      case 'n':
        nodeNum = atoi(optarg);
        break;
      case 'f':
        configFileName = optarg;
        break;
      case 'p':
        startPort = static_cast<unsigned short>(atoi(optarg));
        break;
      default:
        ShowArgsHelp();
        exit(EXIT_FAILURE);
    }
  }

  if (nodeNum <= 0 || configFileName.empty()) {
    ShowArgsHelp();
    exit(EXIT_FAILURE);
  }

  // 清空配置文件，准备由各节点追加写入
  {
    std::ofstream file(configFileName, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
      std::cout << "无法打开 " << configFileName << std::endl;
      exit(EXIT_FAILURE);
    }
    std::cout << configFileName << " 已清空" << std::endl;
  }

  std::cout << "集群起始端口: " << startPort << std::endl;

  for (int i = 0; i < nodeNum; i++) {
    short port = startPort + static_cast<short>(i);
    std::cout << "start to create raftkv node:" << i << "    port:" << port << " pid:" << getpid() << std::endl;
    pid_t pid = fork();  // 创建新进程
    if (pid == 0) {
      // 子进程
      auto kvServer = new KvServer(i, 50000, configFileName, port);
      pause();  // 子进程进入等待状态
    } else if (pid > 0) {
      // 父进程：等1秒再fork下一个节点，避免并发写test.conf冲突
      sleep(1);
    } else {
      std::cerr << "Failed to create child process." << std::endl;
      exit(EXIT_FAILURE);
    }
  }

  // 父进程：等待所有节点都写入了 test.conf，再打印"集群就绪"
  std::cout << "等待所有 " << nodeNum << " 个节点写入配置文件..." << std::endl;
  if (waitForAllNodesReady(configFileName, nodeNum)) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "  集群就绪！可以启动测试客户端了。" << std::endl;
    std::cout << "  配置文件: " << configFileName << std::endl;
    std::cout << "========================================\n" << std::endl;
  } else {
    std::cerr << "警告：等待超时，部分节点可能未成功启动！请检查子进程日志。" << std::endl;
  }

  pause();
  return 0;
}

void ShowArgsHelp() {
  std::cout << "format: command -n <nodeNum> -f <configFileName> [-p <startPort>]" << std::endl;
  std::cout << "  -n  节点数量" << std::endl;
  std::cout << "  -f  配置文件路径" << std::endl;
  std::cout << "  -p  起始端口（可选，默认随机；指定固定端口方便复用test.conf）" << std::endl;
}
