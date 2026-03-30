# 基于raft分布式kv存储

# Raft共识算法

Raft将来自集群外部的客户端请求称之为一条提案proposal，提案在被提交commit之前是不稳定的，需要Raft来让整个集群来决定，产生共识，确定是提交该提案还是抛弃它，此过程中该提案会被打包成为一条日志项log entry，在集群之间传播。在确定可以提交该log entry之后，会将其送入每个节点内部的状态机，成为一个无法被改变的事实。

## 复制状态机

**相同初始状态+相同输入 = 相同结束状态** 

多个节点上从相同数值状态开始，执行相同的一串命令，会产生相同的最终状态。

在Raft中。leader将客户段请求 `command` 封装到一个个日志 `log entry` 中，并将这些`log entries` 复制到所有foller节点，然后节点按照相同顺序应用命令，按照复制状态机的理念，最终结束状态是一样的。

## 状态简化

任何时刻，每一个服务器节点都处于 **leader, follower, candidate这三个状态之一。**

正常状态下，集群中只有一个leader，其余全部都是follower。follower是被动的，它们不会主动发出消息，只是响应leader或candidate的消息，或者转发客户端的请求给leader。candidate是一种临时的状态。

![image.png](image.png)

raft将时间分割成任意长度的任期（**term）任期用连续正数标记**

每一段任期从一次选举开始，如果一次选举无法选出leader，比如收到了相同的票数，此时这一任期就以无leader结束，一个新的任期很快就会重新开始，raft保证一个任期最多只有一个leader。

![image.png](image%201.png)

可以通过任期判断一个服务器的历史状态，比如根据服务器中是否有t2任期内的日志判断其在这个时间段内是否宕机。

服务器节点直接通过RPC通信，有两种RPC：

1. **`RequestVote RPC`** 请求投票，有candidate在选举期发起
2. `AppendEntries RPC` 追加条目，由leader发起，用来复制日志和提供一种心跳机制

服务器之间通信时会 **交换当前任期号** 如果一个服务器上的当前任期小于其他的，就将自己任期更新为较大的那个值。

如果一个候选人或者领导者发现自己任期号过期，则会立刻回到follower状态

**如果一个节点接收到一个包含过期的任期号请求， 直接拒绝**

## 领导者选举

- raft内部有**心跳机制**，leader会周期性向所有follower发送心跳以维持地位， 如果follower**一段时间内**未收到心跳，则可以认为领导者不可用，然后开始选举。
    
    > **follower怎么知道何时应该发起竞选？**
    > 
    
    每个节点内部都有一个被称之为选举时停electionTime的属性，当在此时间段内，没有收到来自leader的心跳消息，就可以认为当前没有leader存在，可以发起竞选。
    
- 开始选举后，follower会 **增加自己的当前任期号** 然后转换到candidate状态，然后 **投票给自己** 并并行的向集群中的其他服务器节点发送投票请求。

选举会产生三种结果：

1. 获得 **超过半数选票** 赢得选举 → 成为leader开始发送心跳
2. 其他节点赢得选举 → 收到**新的心跳**后， 如果 **新leader任期号大于等于自己的任期号** 则从candidate状态回到follower状态
3. 一段时间后没有获胜者， → 每个candidate在一个自己的 **随机选举时间后** 增加任期号开始新一轮投票 

*注：没有获胜者这一情况不需要集群中所有节点达成共识，因为领导者选举成功后会发送心跳*

**follower依据什么向candidate投赞成票**

投票的原则是先来先投票，在一个Term内只能投一次赞成票，如若下一个来请求投票的candidat的Term更大，那么投票者会重置自己的Term为最新然后重新投票。**节点不会向Term小于自身的节点投赞成票，同时日志Index小于自己的也不会赞成**，Term小于自身的消息可能来自新加入的节点，或者那些因为网络延迟而导致自身状态落后于集群的节点，这些节点自然无资格获得赞成票。

```cpp
// 请求投票RPC 请求Request
struct RequestVoteRequest {
	int term; //自己当前任期号
	int candidateId; //自己ID
	int lastLogIndex;//自己最后一个日志号
	int lastLogTerm;//自己最后一个日志任期号
};

//请求投票RPC 响应Response
struct RequestVoteResponse{
	int term; //自己当前任期号
	bool voteGranted; //自己会不会投票给这个候选人
}
```

对于没有成为候选人的follower节点，对同一个任期，会按照 **先来先得** 的原则投票

**选举流程总结：**

- follower长时间未收到来自leader的心跳消息，触发选举时停，转变为candidate，将Term加一，然后将新Term和自身的最新日志序号进行广播以期获取选票。
- 其他收到投票请求的节点先过滤掉Term小于自己的请求，然后判断：1. 自己是否已投票；2. 是否认为现在有leader；3. 该投票请求的日志index是否大于自己。若判断全通过则投赞成票。
- 收到过半数节点的赞成票的candidate将转变成为leader，开始定时发送心跳消息抑制其他节点的选举行为。
- 投票条件被用来保证leader选举的正确性（唯一），随机的选举时停则被用来保证选举的效率，因为集群处于选举状态时是无法对外提供服务的，应当尽量缩短该状态的持续时间，尽量减小竞选失败发生的概率。

## 日志复制

客户端如何得知新leader节点？

1. 访问老leader，如果它还是leader，则直接服务
2. 访问到的是follower， 通过心跳信息，找到leader的ID，告知客户端
3. 如果找到的节点宕机，则只能找其余节点。

当客户端向集群发送一条写请求时，Raft规定只有leader有权处理该请求，如果follower收到该请求则会转发给leader。（如果是candidate则会直接拒绝该请求，因为它认为当前系统不是正常的可提供服务状态）

leader接收到指令就把指令作为新条目追加到日志，一个日志中需要有三个信息：

1. 状态机指令
2. leader任期号
3. 日志号（日志索引） 

![image.png](image%202.png)

*现在我们假设节点1此时发生故障重启，由leader变成follower，此时五个节点开始试图发起竞选。那么哪些节点有资格竞选成功呢？考虑到投赞成票的条件之一是日志需要和投票者相同或者更加新，所以我们就可以确定2、4号节点是不可能赢得选举(当然4号节点有Term落后的原因)，那么1、3、5号节点都有可能胜出，假设5号节点抢先一步获得了2、4节点的选票，那么它将成为leader，**我们可以看到成为leader的必要条件不是拥有最新的日志，而是要拥有最新的共识达成阶段的日志。***

日志号通常是自增且唯一的，但是由于节点可能存在的宕机问题，会出现日志号相同但是日志内容不同的问题，因此， **日志号和任期号两个因素才能唯一确定一个日志**

leader并行发送AppendEntries RPC 给follower 让他们复制条目。当这个条目被半数以上follower复制，则leader可以在 **本地执行该指令并将结果返回给客户端，** 将leader应用日志到状态机这一步称作**提交**。

*注： 在图中logindex为7的日志以及有包括leader在内的超过半数复制，则这个日志可以提交*

在此过程中,leader或follower随时都有崩溃或缓慢的可能性,Raft必须要在有宕机的情况下继续支持日志复制,并且保证每个副本日志顺序的一致(以保证复制状态机的实现)。具体有三种可能:

1. 如果有follower因为某些原因没有给leader响应,那么leader会不断地重发追加条目请求(AppendEntries RPC),哪怕leader已经回复了客户端，也就是说即使leader已经提交了，也不能放弃这个follower，仍然要不断重发，使得follower日志追上leader。
2. 如果有follower崩溃后恢复,这时Raft追加条目的一致性检查生效,保证follower能按顺序恢复崩溃后的缺失的日志。
    - **Raft的一致性检查**: leader在每一个发往follower的追加条目RPC中,会放入**前一个日志条目的索引位置和任期号，**如果follower在它的日志中找不到前一个日志,那么它就会拒绝此日志，leader收到follower的拒绝后，会发送**前一个日志条目，从而逐渐向前定位到follower第一个缺失的日志**。
3. 如果leader崩溃，那么崩溃的leader**可能已经复制了日志到部分follower，但还没有提交**，而被选出的新leader又可能不具备这些日志,这样就**有部分follower中的日志和新leader的日志不相同**。
    - Raft在这种情况下， leader通过**强制follower复制它的日志**来解决不一致的问题，这意味着**follower中跟leader冲突的日志条目会被新leader的日志条目覆盖**(因为没有提交,所以不违背外部一致性)。

通过这种机制，leader在当权后**不需要进行特殊的操作**就能使日志恢复到一致状态。Leader只需要进行正常的操作,然后日志就能在回复AppendEntries一致性检查失败的时候自动趋于一致。

而Leader从来不会覆盖或者删除自己的日志条目。(Append-Only)

通过这种日志复制机制，就可以保证一致性特性：

- 只要过半的服务器能正常运行，Raft就能够接受、复制并应用新的日志条且;
- 在正常情况下，新的日志条目可以在一个RPC来回中被复制给集群中的过半机器；
- 单个运行慢的follower不会影响整体的性能。

![image.png](image%203.png)

说明： 

1. `prevLogIndex` 和 `prevLogTerm` 用于进行一致性检查，只有这两个都与follower中的日志相同，follower才会认为日志一致。
2. follower在接收到leader的日志后不能立刻提交，只有当leader确认日志被复制到大多数节点后，leader才会提交这个日志，也就是应用到自己的状态机，并在RPC请求中通过 `leaderCommit`告知follower，然后follower就可以提交

### **Q:在raft算法中，leader commit后就返回给客户端，此时leader挂了。 flower还没有提交状态，是否会造成安全问题？**

A: 不会。通过**日志提交**规则以及**选举约束**保证

通过提交规则可以确定，此时leader以及确定当前日志被超过半数follower收到。而在leader给客户端提交后，还未广播就宕机，此时follower在心跳超时后开始选举，此时能赢得选举的必然是拥有这个日志的follower，因为这个日志是最新的，没有这个日志的follower不会赢得选票。

新leader在上任后，选举成功之后，新leader会立即广播一条空提案（`lastLogIndex、lastLogTerm`）（no-op机制），试图借此复制之前的所有日志到follower中，以期获得回应。而follower接收到以后，如果没有这个日志，就会拒绝这个RPC，leader会发送再前一个，以期使follower日志与leader同步；如果有这个日志，就返回给leader成功信息。

此时由于这个日志使大多数拥有的，所以leader可以知道这个日志是可以提交的，但是**新 leader 不能直接提交前任 leader 任期内的日志，**所以新 leader 先同步一条 “空日志”，待这条日志被大多数拥有 后，leader发送同步广播，即可安全提交所有≤该索引的、已被半数复制的前任日志。

## 安全性

定义规则使得算法在各类宕机问题下都不会出错：

- **leader** 宕机处理：被选举出的新leader一定包含了之前各任期的所有被提交的日志条目。
- **选举安全**：在一次任期内最多只有一个领导者被选出
- **leader 只添加操作**：领导者在其日志中只添加新条目，不覆盖删除条目
- **日志匹配**：如果两个log包含拥有相同索引和任期的条目，那么这两个log从之前到给定索引处的所有日志条目都是相同的
- **leader完整性**：如果在给定的任期中提交了一个日志条目，那么该entry将出现在所有后续更高任期号的领导者的日志中
- **状态机安全性**: 如果服务器已将给定索引处的日志条目应用于其状态机，则其他服务器不能在该索引位置应用别的日志条目

## 集群成员变更

在需要改变集群配置的时候（例如增减节点，替换宕机的机器），raft可以进行配置变更自动化。

在此过程中，需要防止转换过程中在一个任期内出现两个leader。

![image.png](image%204.png)

原本集群中有1、2、3三个节点，随后加入了4、5两个节点。集群有新成员的加入首先会通知当前leader，加入节点3是leader，他得知了成员变更的消息之后，其对于“大多数”的理解发生了改变，也就是说他现在认为当前集群成员共5个，3个成员才可以视为一个多数派。但是，在节点3将此信息同步给1、2之前，1、2节点仍然认为此时集群的多数派应该是2个。假如不幸地，集群在此时发生分区，1、2节点共处一个分区，剩下的节点在另一个分区。那么1、2节点会自己 开始选举，且能够选出一个合法的leader，只要某个节点获得2票即可。在另一边，也可以合法选出另一个leader，因为它有收获3票的可能性。此时一个集群内部出现了两个合法的leader，且它们也都有能力对外提供服务。这种情况称为“脑裂”

### 联合共识

为了解决一次加入多个新节点的问题，raft论文提出了一种称之为“联合共识”的技巧。我们知道如果一次加入多个节点，无法阻止两个多数派无交集的问题，但是只要限定两个多数派不能做出不同决定即可。联合共识的核心思想是引入一种中间状态，即让节点明白此时正处于集群成员变更的特殊时期，让它知晓两个集群的成员情况，

当该节点知道当前处于中间状态时，它的投票必须要同时获得两个集群的多数派的支持。以此保证两个多数派不会做出截然相反的行为。举个例子来说明联合共识的内容。这里我们用新旧配置表示节点对于集群情况的认知（因此成员变更也被成为配置变更），例如旧配置有3个节点的地址和序号，新配置有5个。

<aside>
💡

集群从 C_旧 切换到 C_新 的过程中，**任何需要 “法定人数（多数派）” 确认的操作（选举、日志提交），必须同时获得「C_旧 的多数派」和「C_ 新的多数派」的支持**。

但这个规则的**前提**是：**操作涉及的节点集，需同时覆盖新旧配置**；如果操作仅在**纯旧配置节点**中进行，那么「满足 C_ 旧多数派」就**等价于满足双配置多数派**。

</aside>

## **raft的可用性和一致性**

### 只读请求

对于客户端的只读请求,我们可以有以下几种方法进行处理:

- **follower自行处理**:一致性最低,最无法保证,效率最高。
- **leader立即回复**:leader可能并不合法(分区),导致数据不一致。
- **将读请求与写请求等同处理**:保证强一致,但性能最差。
- **readIndex方法**:由leader处理,处理之前首先向集群发送确认信息保证自己是合法leader,随后等待状态机已将请求到来时的commitIndex应用(说明请求到来之前的所有操作都已被状态机执行),才能回复读请求。保证一致,性能较好,是etcd建议的一种方式。
- **LeaseRead方法**:Leader取一个比ElectionTimeout小的租期,在租期内不会发生选举,确保Leader不会变,即可跳过群发确认消息的步骤。性能较高,一致性可能会受服务器时钟频率影响。

对于读请求的处理能够让我们明显地看出共识算法和分布式系统一致性之间的关系。当共识达成时,即leader已将某entry提交,则整个系统已经具备了提供强一致服务的能力。但是,可能由于状态机执行失败、选择其他读请求处理方案等因素,导致在外部看来系统是不一致的。

### 网络分区

节点数量超过 n/2+1的一边可以继续提供服务，即使可能没有leader，也可以通过再次选举产生。而另一边的旧leader并不能发挥其作用，因为它并不能联系到大多数节点。

问题：raft属于A型还是C型？我们认为raft是一个强一致、高可用的共识算法。即使网络分区发生，只要由半数以上的节点处于同一个分区（假设60%），那么系统就能提供60%的服务，并且还能保证是强一致的。

### 预选举

由于接收不到leader的心跳，它们会自己开始选举，但由于人数过少不能成功选举出leader，所以会一直尝试竞选。它们的term可能会一直增加到一个危险的值。但分区结束时，通过心跳回应，原leader知道了集群中存在一群Term如此大的节点，它会修改自己的Term到已知的最大值，然后变为follower。此时集群中没有leader，需要重新选举。但是那几个原先被隔离的节点可能由于日志较为落后，而无法获得选举，新leader仍然会在原来的正常工作的那一部分节点中产生。

虽说最终结果没错，但是选举阶段会使得集群暂时无法正常工作，应该尽量避免这种扰乱集群正常工作的情况的发生，所以raft存在一种预选举的机制。真正的选举尽管失败还是会增加自己的Term，预选举失败后会将term恢复原样。只有预选举获得成功的candidate才能开始真正的选举。这样就可以避免不必要的term持续增加的情况发生。

### 快照

长时间运作的集群，其内部的日志会不断增长。因此需要对其进行压缩，并持久化保存。对于新加入集群的节点，可以直接将压缩好的日志（称之为快照Snapshot）发送过去即可。

Raft使用的是一种快照机制，在每个节点达到一定条件后，可以把当前日志中的命令都写入快照，然后就可以把已经并入快照的日志删除。

此时如果一个节点长时间宕机或者新加入集群，那么leader直接向follower发送自己的快照

# 项目示意图

![image.png](image%205.png)

# clerk 客户端代码解析

clerk是一个外部的客户端，用于向整个raft集群发起命令并接收相应。

**clerk 类代码：**

```cpp
class Clerk {
 private:
  std::vector<std::shared_ptr<raftServerRpcUtil>> m_servers;  //保存所有raft节点的fd
  std::string m_clientId;
  int m_requestId;
  int m_recentLeaderId;  //只是有可能是领导

  std::string Uuid() {
    return std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand());
  }  //用于返回随机的clientId

  //    MakeClerk  todo
  void PutAppend(std::string key, std::string value, std::string op);

 public:
  //对外暴露的三个功能和初始化
  void Init(std::string configFileName);
  std::string Get(std::string key);

  void Put(std::string key, std::string value);
  void Append(std::string key, std::string value);

 public:
  Clerk();
};
```

**clerk调用代码如下：**

```cpp
int main(){
    Clerk client;
    client.Init("test.conf");
    auto start = now();
    int count = 500;
    int tmp = count;
    while (tmp --){
        client.Put("x",std::to_string(tmp));
        std::string get1 = client.Get("x");
        std::printf("get return :{%s}\r\n",get1.c_str());
    }
    return 0;
	}
```

## 调用代码分析：

1. 初始化

客户端从一个config文件中读取了所有raft节点的ip和端口号，用于初始化客户端与其他raft节点的链接，从代码中可以看出，初始化实际上是将节点的ip与端口传入 `raftServerRpcUtil` 在这里面封装了**`kvServerRpc_Stub`** 也就是节点用于和上层状态机通信的服务器。

由rpc的知识可知，stub类的初始化是需要一个重写的channel类作为参数，在重写的mprpcChannel类中，设置了一个clientFd的成员函数，用以保存客户端用于和节点通信的socket。

```cpp

//clerk中的属性
std::vector<std::shared_ptr<raftServerRpcUtil>> m_servers;  //保存所有raft节点的fd

//clerk中链接所有raft节点的过程
for (const auto& item : ipPortVt) {
    std::string ip = item.first;
    short port = item.second;
    m_servers.push_back(std::make_shared<raftServerRpcUtil>(ip, port));
     //make_shared 提供了更高效、更安全的方式来构造 shared_ptr。它不仅优化了内存分配、减少了内存泄漏的风险，还能提升缓存的性能。
  }
  
  //rpcUtil的构造
  raftServerRpcUtil::raftServerRpcUtil(std::string ip, short port) {
  stub = new raftKVRpcProctoc::kvServerRpc_Stub(new MprpcChannel(ip, port, false));
}

//具体的rpc链接
MprpcChannel::MprpcChannel(string ip, short port, bool connectNow) : m_ip(ip), m_port(port), m_clientFd(-1) {
  if (!connectNow) {
    return;
  }  //可以允许延迟连接
  std::string errMsg;
  //通过调用newConnect函数，得到了一个与<ip,port>通信的clientFd
  auto rt = newConnect(ip.c_str(), port, &errMsg);
  int tryCount = 3;
  while (!rt && tryCount--) {
    std::cout << errMsg << std::endl;
    rt = newConnect(ip.c_str(), port, &errMsg);
  }
```

1. putAppend函数

`void **Clerk**::**PutAppend**(**std**::**string** key, **std**::**string** value, **std**::**string** op)`

将参数初始化后，调用 `m_server` 中stub类的方法然后检查返回值。这两个方法由于是rpc方法，具体的使用在server端详细介绍。

# KVserver代码解析

kvserver实际上是一个中间组件，负责沟通kvDB和raft节点，同时也会接收外部的请求完成相应。

**在该实现中，每个  `KVServer` 实例内部都包含一个 `Raft`节点。`KVServer` 负责对外提供 KV RPC 接口，并将请求提交给本地 Raft 节点进行一致性复制；Raft 在日志提交后通过 apply 通道回调 `KVServer` 执行状态机。两者运行在同一进程中，但在逻辑上严格分层。**

## KVServer初始化：启动了一个KVSever+一个Raft节点

```cpp
KvServer::KvServer(int me, int maxraftstate,
                   std::string nodeInforFileName, short port)
{
		m_me = me;
		m_maxRaftState = maxraftstate;
		
		//Raft → KVServer 的 apply 通道
		applyChan = std::make_shared<LockQueue<ApplyMsg>>();
		//Raft 节点对象
		m_raftNode = std::make_shared<Raft>();
	
		//启动RPC服务线程
		std::thread t([this, port]() {
		RpcProvider provider;
	  provider.NotifyService(this);              // KVServer RPC
	  provider.NotifyService(m_raftNode.get());  // Raft RPC
	  provider.Run(m_me, port);
		});
		t.detach();
		//此时对外提供KVserver服务，对内作为一个Raft节点，提供与其他节点的rpc通信
		//作为server的部分结束，分离后作为守护线程持续提供服务
		
		
		//作为raft节点部分，需要链接其余raft节点
		for (int i = 0; i < ipPortVt.size(); ++i) {
    if (i == m_me) {
      servers.push_back(nullptr);
      continue;
    }
    std::string otherNodeIp = ipPortVt[i].first;
    short otherNodePort = ipPortVt[i].second;
    auto *rpc = new RaftRpcUtil(otherNodeIp, otherNodePort);
    servers.push_back(std::shared_ptr<RaftRpcUtil>(rpc));

    std::cout << "node" << m_me << " 连接node" << i << "success!" << std::endl;
  }
  sleep(ipPortVt.size() - me);  //等待所有节点相互连接成功，再启动raft
  m_raftNode->init(servers, m_me, persister, applyChan);
  
  //等待raft传来applychan
  std::thread t2(&KvServer::ReadRaftApplyCommandLoop, this);
  t2.join();  //由於ReadRaftApplyCommandLoop一直不會結束，达到一直卡在这的目的
}
```

## 如何处理外部请求：通过rpc

外部请求分为两种，get和put分别对应两种rpc服务

### Get()请求

整个请求流程图如图

```cpp
Clerk
  |
  | Get(key)
  v
KvServer::Get()         （RPC 线程）
  |
  | raft->Start(op)
  v
Raft Leader
  |
  | AppendEntries
  v
Followers
  |
  | 多数确认
  v
Raft commit
  |
  | ApplyMsg(index, op)
  v
KvServer::ApplyLoop()   （Apply 线程）
  |
  | 执行状态机
  | 唤醒等待者
  v
KvServer::Get() 返回
  |
  v
Clerk 收到 value

```

客户端通过rpc将请求发送至KVserver，server本地执行Get：

1. 反序列化参数

```cpp
Op op;
op.type = GET;
op.key = key;
op.clientId = clientId;
op.seq = seq;
```

1. 将请求交给Raft集群

```cpp
m_raftNode->S2.start(op, &raftIndex, &_, &isLeader);
```

1. server中开始等待raft集群的结果，开始阻塞

这里的raftIndex是表示这个操作的日志号

```cpp
m_mtx.lock();
  if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
    waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
  }
  auto chForRaftIndex = waitApplyCh[raftIndex];
  m_mtx.unlock();  //直接解锁，等待任务执行完成，不能一直拿锁等待
  Op raftCommitOp;
  if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) 
```

1. raft内部自动执行，将执行结果写入管道
- 服务端向 Raft Leader 发送请求，Leader 将请求封装为 LogEntry 写入本地日志；
- Leader 通过 AppendEntries（心跳 / 日志同步）将日志同步到半数以上 Follower；
- Leader 标记该日志为 “已提交”，更新自己的`commitIndex`，并通过心跳把`commitIndex`同步给所有 Follower；
- **所有节点**的`applierTicker`定时轮询，发现`lastApplied <= commitIndex`，触发日志应用 **`getApplyLogs`** 这个函数将所有 ****`lastApplied < commitIndex` 的日志进行提交，将这些可以提交的日志存放到applymsg中，程序如下：

```cpp
std::vector<ApplyMsg> Raft::getApplyLogs() {
  std::vector<ApplyMsg> applyMsgs;
 
  while (m_lastApplied < m_commitIndex) {
    m_lastApplied++;
    ApplyMsg applyMsg;
    applyMsg.CommandValid = true;
    applyMsg.SnapshotValid = false;
    applyMsg.Command = m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].command();
    applyMsg.CommandIndex = m_lastApplied;
    applyMsgs.emplace_back(applyMsg);
   
  }
  return applyMsgs;
}

void Raft::applierTicker() {
  while (true) {
    m_mtx.lock();
    if (m_status == Leader) {
      DPrintf("[Raft::applierTicker() - raft{%d}]  m_lastApplied{%d}   m_commitIndex{%d}", m_me, m_lastApplied,
              m_commitIndex);
    }
    auto applyMsgs = getApplyLogs();
    m_mtx.unlock();
    for (auto& message : applyMsgs) {
      applyChan->Push(message);
    }
    sleepNMilliseconds(ApplyInterval);
  }
}
```

- 所有节点的`applierTicker`将日志解析操作，将可以commit的日志操作放入applychan中，而在server中正在等待对应日志的结果。
1. server在初始化时有 **`ReadRaftApplyCommandLoop`** 线程持续在后台通过阻塞队列监听raft与server的通信管道，如果有了信息，就通过 `GetCommandFromRaft` 函数执行，这个函数中将根据日志中的指令完成server与KV数据库的交互，然后通过函数 `SendMessageToWaitChan` 将结果放在以该任务日志号为索引的消息管道中，此时一直在监听的rpc线程监听到队列中有了数据，开始读取处理。

```cpp
void KvServer::ReadRaftApplyCommandLoop() {
  while (true) {
    auto message = applyChan->Pop();  //阻塞弹出
    if (message.CommandValid) {
      GetCommandFromRaft(message);
    }
  }
}

void KvServer::GetCommandFromRaft(ApplyMsg message) {
  Op op;
  op.parseFromString(message.Command);

  if (message.CommandIndex <= m_lastSnapShotRaftLogIndex) {
    return;
  if (!ifRequestDuplicate(op.ClientId, op.RequestId)) {
    if (op.Operation == "Put") {
      ExecutePutOpOnKVDB(op);
    }
    if (op.Operation == "Append") {
      ExecuteAppendOpOnKVDB(op);
    }
  }
  
  SendMessageToWaitChan(op, message.CommandIndex);
}

bool KvServer::SendMessageToWaitChan(const Op &op, int raftIndex) {
  std::lock_guard<std::mutex> lg(m_mtx);
  
  if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
    return false;
  }
  waitApplyCh[raftIndex]->Push(op);
  
  return true;
}
```

1. rpc线程唤醒后的操作

```cpp
Op raftCommitOp;
if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) {
  // 超时分支处理
} else {
  // 正常拿到通知，非超时分支处理
}
```

Get 请求**阻塞等待**专属队列中的数据，最长等待`CONSENSUS_TIMEOUT`时间：

- 若**超时未拿到数据**（`timeOutPop返回false`）：说明 Get 请求等待超时，可能是 Leader 身份漂移、网络阻塞、Raft 处理缓慢等原因，进入超时容错分支。

```cpp
if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) {
  // 1. 重新获取当前节点的Raft状态（Leader/非Leader）
  int _ = -1;
  bool isLeader = false;
  m_raftNode->GetState(&_, &isLeader);

  // 2. 幂等性校验：检查该Get请求是否是已经处理过的重复请求
  if (ifRequestDuplicate(op.ClientId, op.RequestId) && isLeader) {
    // 2.1 是重复请求且还是Leader → 直接读跳表返回结果
    std::string value;
    bool exist = false;
    ExecuteGetOpOnKVDB(op, &value, &exist);
    if (exist) {
      reply->set_err(OK);
      reply->set_value(value);
    } else {
      reply->set_err(ErrNoKey);
      reply->set_value("");
    }
  } else {
    // 2.2 不是重复请求/已不是Leader → 返回ErrWrongLeader，让客户端重试
    reply->set_err(ErrWrongLeader);
  }
} 
```

**（1）：重新获取 Raft 状态（`GetState`）**

**目的**：超时的核心原因可能是**Leader 身份漂移**（比如 Leader 在等待期间被新 Leader 取代），需要**重新校验当前节点是否还是 Leader**，不能使用之前的 `isLeader` 值（已过期）。

**（2）：幂等性校验（`ifRequestDuplicate(ClientId, RequestId)`）**

Server 会维护一个**已处理请求的缓存表**（ **`unordered_map**<**std**::**string**, int> m_lastRequestId`），记录所有已成功处理的客户端请求，该函数的作用是**检查本次 Get 请求是否是已经处理过的重复请求**。

- **为什么需要幂等性？**客户端在收到 RPC 响应前超时，会**认为请求处理失败**，并向集群**重试该请求**——如果不做幂等性校验，会导致同一个 Get 请求被多次处理，虽然读操作无副作用，但会增加服务器负载，且工程上要求**所有分布式请求必须实现幂等性**（写操作更关键，Put/Append 必须做）。

**（3）：不是重复请求 / 已不是 Leader → 返回 `ErrWrongLeader`**

**设计目的**：若**已不是 Leader**：直接让客户端 Clerk 重新找 Leader 发起请求，这是最安全的容错方式；若**还是 Leader 但不是重复请求**：说明超时是因为 Raft 处理缓慢、网络阻塞等原因，此时返回 `ErrWrongLeader` 让客户端重试，是**工程上的简单有效选择**（避免无限等待）；

- 若**在超时前拿到数据**（`timeOutPop返回true`）：说明 Raft 层已成功确认该 Get 请求的 “提交”，Leader 身份在这段时间**持续有效**，可以安全读跳表；

```cpp
else {
  // 校验：当前唤醒的Op是否是本次Get请求的原始Op
  if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId) {
    // 校验通过 → 正式读跳表
    std::string value;
    bool exist = false;
    ExecuteGetOpOnKVDB(op, &value, &exist);
    if (exist) {
      reply->set_err(OK);
      reply->set_value(value);
    } else {
      reply->set_err(ErrNoKey);
      reply->set_value("");
    }
  } else {
    // 校验失败 → 返回Leader错误，让客户端重试
    reply->set_err(ErrWrongLeader);
  }
}
```

（1）：Op 一致性校验（`ClientId + RequestId`）

**目的**：确保从阻塞队列中拿到的raftCommitOp**是本次 Get 请求对应的 Op**，而非其他请求的 Op（防止消息乱序、raftIndex 映射错误、Raft 层推送错误等问题）。

可能的异常场景：

- 多线程并发下，Raft 层的 applier 线程将**其他请求的 Op**错误推入了当前队列；
- raftIndex 的映射关系在`waitApplyCh`中出现错误（虽然理论上不会，工程上做双重保障）。通过`ClientId + RequestId`的全局唯一标识，能 100% 确保 Op 的一致性，避免处理错误的请求。

（2）：校验通过 → 执行实际读操作（`ExecuteGetOpOnKVDB`）

（3)：校验失败 → 返回`ErrWrongLeader`

**设计目的**：校验失败说明出现了**不可预期的异常**（比如消息乱序、Raft 层错误），此时最安全的处理方式是**快速失败**，让客户端重试，避免返回错误的数据，保证系统的**正确性优先**。

(4)：清理`waitApplyCh`的资源（防止内存泄漏 + 保证 map 整洁）

这是 Get 操作的**收尾工作**，无论超时还是非超时，最终都会执行，清理为本次请求创建的临时资源。

```cpp
m_mtx.lock();  
auto tmp = waitApplyCh[raftIndex];
waitApplyCh.erase(raftIndex);
delete tmp;
m_mtx.unlock();
```

### Get总结：

1.  保证 Get 操作的**线性一致性**
- 强制 Leader 只读，两道 Leader 身份校验（`Start`后 + 超时后）；
- 阻塞等待 Raft 层的提交通知，确保 Leader 身份在处理期间持续有效；
- 只有 Raft 层确认有效后，才允许读本地跳表。
1. 解决分布式系统的**常态问题**
- **超时问题**：通过`timeOutPop`实现超时容错，快速失败；
- **重试问题**：通过`ClientId + RequestId`实现**幂等性校验**，避免重复处理；
- **Leader 身份漂移**：多次校验 Leader 状态，漂移后让客户端重试；
- **消息乱序**：Op 一致性校验，确保处理正确的请求。

Get 操作并非 “简单的本地读”，而是 **经过 Raft 流程校验的安全本地读**——走 Raft 的流程，不走 Raft 的日志共识，既保证了线性一致性，又兼顾了读操作的性能，同时解决了分布式场景下的各种容错问题，是Raft 入门项目中非常标准的工业级 Get 操作实现 。

### putAppend()请求

put请求，也就是写请求前期的流程与get一样

不同的是 ： 在`GetCommandFromRaft` 函数中，直接完成了对数据库的写入操作，后续rpc的请求线程无论是返回成功或是超时失败等都不会影响结果，因为只要才raft中完成日志的同步提交，这个日志就会落地，而客户端如果收到错误回复重新请求的时候，服务端会通过幂等性校验，自动重试等方式使得客户端拿到最终正确的结果。

- 为什么要这么设计：

当日志在集群中已经被提交后，这条日志必然会被所有节点同步并且操作于数据库中，而rpc线程不必等待写入操作完成，否则在高并发的状态下rpc线程可能会很快因为等待io而使得线程池耗尽。

### Q:如果客户端发出的写请求已经成功提交，但是rpc线程由于网络问题发出超时问题，客户端再次想请求写指令的时候leader已经更换了，此时已经是另一个服务端了，如何通过幂等判断

### A:只要原请求的日志在旧 Leader 上成功提交，Raft 协议会保证新 Leader 必然同步并落地这份日志，而项目中「全集群统一的幂等标识（ClientId+RequestId）+ **集群节点维护请求执行记录**
**两个保障**

1. ：Raft 日志成功提交 → 全集群所有节点最终都会同步并落地该日志

Raft 协议的**核心共识保障**：

**只要旧 Leader 将日志同步到集群半数以上节点并标记为提交,这份日志就成为集群的「全局已提交日志」**,无论后续 Leader 是否更换,**新 Leader 必然会持有这份日志**(新 Leader 是从集群节点中选举出来的,选举的前提是该节点持有「集群最新的已提交日志」,否则拿不到半数选票)。且新 Leader 当选后,会通过 Raft 的日志同步机制,让集群中剩余节点(包括自己)快速追平所有已提交日志,最终**全集群所有节点的跳表都会落地该日志对应的写操作**—— 这是跨 Leader 幂等判断的**数据基础**。

1. ：幂等标识( `ClientId+RequestId`)是「全局唯一」的,与 Leader 无关

项目中定义的**幂等核心标识** `ClientId + RequestId`,是**客户端级别的全局唯一标识**,而非 Leader 级别的：

- `ClientId`：客户端的唯一标识(比如客户端启动时生成的 UUID / 自增 ID),一个客户端一生只有一个；
- `RequestId`：客户端为每个请求分配的**单调递增 ID**(比如从 1 开始,每发一个请求 + 1)；
- 两者组合：能**唯一标识客户端的某一个具体请求**,无论这个请求发给哪个 Leader、重试多少次,标识都不会变 —— 这是跨 Leader 幂等判断的**标识基础**。

## raftCore 代码解析

### Init函数

这个函数主要初始化节点的状态信息，初始化当前索引为0，已提交的状态为0等，同时会启动三个定时器

```cpp
  m_ioManager->scheduler([this]() -> void { this->leaderHearBeatTicker(); });
  m_ioManager->scheduler([this]() -> void { this->electionTimeOutTicker(); });
  m_ioManager->scheduler([this]() -> void { this->applierTicker(); });
```

分别是领导者的心跳计时器，日志超时选举计时器以及日志同步计时器。

### 选举流程以及主要函数：

![image.png](image%206.png)

1. **electionTimeOutTicker：负责检查是否发起选举，如果是时候则执行doElection**

定时器设置死循环一直运转， 如果是leader状态， 无需参与选举，则空转一个 `heartbeat` 时间，避免空转。 对于follower节点，计算当前需要睡眠时间：

**本次需要睡眠的时长 = 随机选举超时时间 + 上一次收到心跳包的时间 -  现在的时间**

然后使用usleep进行睡眠，在睡眠结束后进行检查: 在此期间是否有收到心跳包，如果收到心跳包，   `m_lastResetElectionTime` 会更新，结果会大于0，就继续睡眠，如果超时未收到，则会执行选举操作。

```cpp
if (std::chrono::duration<double,std::milli>(m_lastResetElectionTime-wakeTime).count()>0) {
  // 说明睡眠的这段时间有重置定时器，那么就没有超时，再次睡眠
  continue;
}
doElection(); //进行选举
```

1. **doElection**()

```cpp
void Raft::doElection() {
  std::lock_guard<std::mutex> g(m_mtx);
  if (m_status != Leader) {
    // 当选举的时候定时器超时就必须重新选举，不然没有选票就会一直卡主
    // 重竞选超时，term也会增加的
    m_status = Candidate;
    /// 开始新一轮的选举
    m_currentTerm += 1;
    m_votedFor = m_me;  // 即是自己给自己投，也避免candidate给同辈的candidate投
    persist();
    std::shared_ptr<int> votedNum = std::make_shared<int>(1);  // 使用 make_shared 函数初始化 !! 亮点
    //	重新设置定时器
    m_lastResetElectionTime = now();
    //	发布RequestVote RPC
    for (int i = 0; i < m_peers.size(); i++) {
      if (i == m_me) {
        continue;
      }
      int lastLogIndex = -1, lastLogTerm = -1;
      getLastLogIndexAndTerm(&lastLogIndex, &lastLogTerm);  // 获取最后一个log的term和下标
      std::shared_ptr<raftRpcProctoc::RequestVoteArgs> requestVoteArgs =
          std::make_shared<raftRpcProctoc::RequestVoteArgs>();
      requestVoteArgs->set_term(m_currentTerm);
      requestVoteArgs->set_candidateid(m_me);
      requestVoteArgs->set_lastlogindex(lastLogIndex);
      requestVoteArgs->set_lastlogterm(lastLogTerm);
      auto requestVoteReply = std::make_shared<raftRpcProctoc::RequestVoteReply>();

      // 使用匿名函数执行避免其拿到锁
      std::thread t(&Raft::sendRequestVote, this, i, requestVoteArgs, requestVoteReply,
                    votedNum);  // 创建新线程并执行b函数，并传递参数
      t.detach();
    }
  }
}
```

在选举函数中，首先将自己的状态改为候选人，自己任期+1，自己给自己投票，然后构造发送给其余节点的请求投票的日志，上面带有自己的任期，自己的id，自己上一个日志的任期和id，创建线程调用 `sendRequestVote` 将日志发给每一个节点。

1. `sendRequestVote` 

        主要代码：

```cpp
bool ok = m_peers[server]->RequestVote(args.get(), reply.get());

if (!ok) {
    return ok;  
  }

if (reply->term() > m_currentTerm) {
    m_status = Follower;  // 三变：身份，term，和投票
    m_currentTerm = reply->term();
    m_votedFor = -1;
    persist();    
    return true;
  } else if (reply->term() < m_currentTerm) {
    return true;
  }
  
  *votedNum = *votedNum + 1;
  if (*votedNum >= m_peers.size() / 2 + 1) {
			  *votedNum = 0;
    if (m_status == Leader) {
      // 如果已经是leader了，那么是就是了，不会进行下一步处理了k
      myAssert(false,
               format("[func-sendRequestVote-rf{%d}]  term:{%d} 同一个term当两次领导，error", m_me, m_currentTerm));
    }
    //	第一次变成leader，初始化状态和nextIndex、matchIndex
    m_status = Leader;
    int lastLogIndex = getLastLogIndex();
    for (int i = 0; i < m_nextIndex.size(); i++) {
      m_nextIndex[i] = lastLogIndex + 1;  // 有效下标从1开始，因此要+1
      m_matchIndex[i] = 0;                // 每换一个领导都是从0开始，见fig2
    }
    std::thread t(&Raft::doHeartBeat, this);  // 马上向其他节点宣告自己就是leader
    t.detach();

    persist();
  }
```

函数中调用了RPC函数，将请求投票的信息发送并收到回复。其中返回的ok表示该网络通信是否成功，如果通信不成功访问后续的reply会出问题。

然后当成功返回后，判断reply中任期与当前节点的关系：

- 如果大于当前节点：说明当前节点任期落后，不满足选举条件，需要将状态转换为follower，当前任期更新，投票状态改变。
- 如果小于当前节点，直接返回，不进行投票。

**原因：**Raft 协议规定：**节点只处理和当前任期匹配的请求 / 回复**，低任期的信息是 “过时的”，既不需要更新自己的状态，也不需要统计投票（因为当前选举是新任期的，旧任期的投票数无效）。如果使用了旧任期的选票，可能出现跨任期当选的情况，发生集群脑裂问题：

```cpp
- A 在任期 5 发起选举，没拿到足够票数；
- A 更新到任期 6，重新发起选举；
- 此时收到任期 5 的投票回复，统计后 “凑够了票数”，A 认为自己在任期 6 当选 Leader；
- 但其他节点可能在任期 6 已经选举出了另一个 Leader，导致集群有两个 Leader，数据一致性被破坏。
```

如果成功获得大多数选票，则需要进行一些初始化操作：

- 投票数置零
- 根据自己的最后一条日志，初始化m_nextIndex，也就是leader希望其他节点下一个接收的日志索引
- 将已匹配日志置零m_matchIndex。

然后发送心跳，宣布自己是领导者。

1. `RequestVote()` 

这个函数是用于决定是否投票。

```cpp
void Raft::RequestVote(const raftRpcProctoc::RequestVoteArgs* args, raftRpcProctoc::RequestVoteReply* reply) {
  std::lock_guard<std::mutex> lg(m_mtx);

  // 对args的term的三种情况分别进行处理，大于小于等于自己的term都是不同的处理
  //  reason: 出现网络分区，该竞选者已经OutOfDate(过时）
  if (args->term() < m_currentTerm) {
    reply->set_term(m_currentTerm);
    reply->set_votestate(Expire);
    reply->set_votegranted(false);
    return;
  }
  // fig2:右下角，如果任何时候rpc请求或者响应的term大于自己的term，更新term，并变成follower
  if (args->term() > m_currentTerm) {
    m_status = Follower;
    m_currentTerm = args->term();
    m_votedFor = -1;

    //	重置定时器：收到leader的ae，开始选举，透出票
    // 这时候更新了term之后，votedFor也要置为-1
  }
  //	现在节点任期都是相同的(任期小的也已经更新到新的args的term了)，还需要检查log的term和index是不是匹配的了

  int lastLogTerm = getLastLogTerm();
  // 只有没投票，且candidate的日志的新的程度 ≥ 接受者的日志新的程度 才会授票
  if (!UpToDate(args->lastlogindex(), args->lastlogterm())) {
    // args.LastLogTerm < lastLogTerm || (args.LastLogTerm == lastLogTerm && args.LastLogIndex < lastLogIndex) {
    // 日志太旧了
    reply->set_term(m_currentTerm);
    reply->set_votestate(Voted);
    reply->set_votegranted(false);
    return;
  }
  // 啥时候会出现rf.votedFor == args.CandidateId ，就算candidate选举超时再选举，其term也是不一样的呀
  //     当因为网络质量不好导致的请求丢失重发就有可能！！！！
  if (m_votedFor != -1 && m_votedFor != args->candidateid()) {
    reply->set_term(m_currentTerm);
    reply->set_votestate(Voted);
    reply->set_votegranted(false);
    return;
  } else {
    m_votedFor = args->candidateid();
    m_lastResetElectionTime = now();  // 认为必须要在投出票的时候才重置定时器，
    reply->set_term(m_currentTerm);
    reply->set_votestate(Normal);
    reply->set_votegranted(true);

    return;
  }
}
```

### 日志复制与心跳机制主要函数：

![image.png](image%207.png)

1. leaderHeartBeatTicker:负责检查是否要发送心跳，如果发起就执行heartbeat

计算睡眠时间的逻辑与选举超时计时器很像

```cpp
std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
    std::chrono::system_clock::time_point wakeTime{};
    {
      std::lock_guard<std::mutex> lock(m_mtx);
      wakeTime = now();
      suitableSleepTime = std::chrono::milliseconds(HeartBeatTimeout) + m_lastResetHearBeatTime - wakeTime;
    }
```

假设`HeartBeatTimeout=50ms`，`m_lastResetHearBeatTime`是上一次重置心跳的时间（比如 T0）；本次应休眠的时间 = 50ms - (当前时间 - T0) → 保证从 T0 开始，每 50ms 触发一次心跳；

- 例子：T0=1000ms，当前时间 = 1030ms → 应休眠 20ms（1030+20=1050=1000+50），刚好凑够 50ms 间隔；
- **作用**：精准计算 “还需要休眠多久”，而不是固定休眠 50ms（避免累计误差）。

```cpp
if (std::chrono::duration<double, std::milli>(m_lastResetHearBeatTime - wakeTime).count() > 0) {
  // 睡眠的这段时间有重置定时器，没有超时，再次睡眠
  continue;
}
doHeartBeat();
```

如果期间有重置心跳时间，就再次休眠，如果没有就发送心跳包

**重置心跳时间的场景**：`m_lastResetHearBeatTime`是 “心跳定时器的基准时间”，通常在以下场景被重置：

1. Leader 刚当选时，初始化该值为当前时间；
2. 手动触发心跳时（比如`doHeartBeat()`执行后），重置该值；
3. 发送日志同步 RPC 时（心跳复用`AppendEntries`），重置该值；→ 核心目的：保证 “心跳间隔” 是从 “最后一次发送心跳 / 日志” 开始计算，而非固定时间点。
1. `doHeartBeat()` 

完整逻辑流程：

1. 检查是否需要发送快照：
    - 如果领导者发送给追随者的 `nextIndex` 小于快照的最后一条日志索引 `m_lastSnapshotIncludeIndex` :
        - 说明追随者的日志与 Leader 相差较远,缺少的日志已经被 Leader 快照覆盖。此时 Leader 创建一个线程来发送快照(`leaderSendSnapShot`),并跳过当前追随者的日志发送逻辑。
2. 检查日志的同步范围：
    - 如果追随者的 `preLogIndex`(即拥有的最后一条日志索引)不等于快照的 `m_lastSnapshotIncludeIndex` :
        - 说明追随者的日志覆盖范围超出了快照,可以从 `preLogIndex+1` 开始,直到 m_logs 的末尾。将这段日志依次添加到 `AppendEntriesArgs`中。
    - 如果 `preLogIndex`恰好等于 `m_lastSnapshotIncludeIndex`:
        - 说明追随者的日志正好停留在快照范围内。此时,Leader 应该从快照后的日志开始,依次将 m_logs 中的条目添加到 AppendEntriesArgs 中。
3. 构建返回值：
    - 使用 `AppendEntriesReply`构造初始返回值,设置默认状态(如Disconnected)。构建的RPC请求和响应将交由 `sendAppendEntries`函数处理，该函数会在单独的线程中执行，负责：
        - 发送RPC请求
        - 处理RPC响应
4. 重置心跳计时器：

发送心跳后,Leader会重置心跳计时器 `m_lastRestElectionTime` ,确保心跳周期正常继续。

1. **`sendAppendEntries()`** 

主要流程：

```cpp
1. 调用 RPC -> ok? -> 失败则返回
2. 检查 reply->appstate -> Disconnected? -> 返回
3. 检查 reply->term:
    - 大于当前 term -> 降级为 Follower
    - 小于当前 term -> 忽略返回
4. 如果不是 Leader -> 直接返回
5. 检查 reply->success:
    - false -> 回退 nextIndex 并重试
    - true:
    a. 更新 matchIndex 和 nextIndex
    b. 增加 appendNums
6. 检查提交条件:
    - 超过半数追随者同步 -> 更新 commitIndex
7. 返回 ok
```

首先是发送rpc向其他节点，得到返回后，需要检查reply中的term与自身的关系。在检查完成后。如果reply中标志不成功，则说明之前发送的日志超前了与目标节点的日志不匹配，需要回退nextIndex。

```cpp
if (!reply->success()) {
      m_nextIndex[server] = reply->updatenextindex();  // 失败是不更新mathIndex的
    }
```

如果reply成功，则需要更新 该节点的matchIndex 和 nextIndex，并且统计收到这一日志的节点数

```cpp
else {
    *appendNums = *appendNums + 1;
    
    // 更新matchIndex：取当前值和“本次同步的最后索引”的最大值（避免重复更新）
    m_matchIndex[server] = std::max(m_matchIndex[server], args->prevlogindex() + args->entries_size());
    m_nextIndex[server] = m_matchIndex[server] + 1;
    int lastLogIndex = getLastLogIndex();

    if (*appendNums >= 1 + m_peers.size() / 2) {
      *appendNums = 0;   
      // leader只有在当前term有日志提交的时候才更新commitIndex，因为raft无法保证之前term的Index是否提交
      // 只有当前term有日志提交，之前term的log才可以被提交，只有这样才能保证“领导人完备性
      //{当选领导人的节点拥有之前被提交的所有log，当然也可能有一些没有被提交的}”    
      if (args->entries_size() > 0 && args->entries(args->entries_size() - 1).logterm() == m_currentTerm) {   
        m_commitIndex = std::max(m_commitIndex, args->prevlogindex() + args->entries_size());
      }
    }
  }
```

Q: 为什么 `args->entries(args->entries_size() - 1).logterm() == m_currentTerm` 时，才能更新commitIndex

A: 因为leader不能提交之前trem的日志，而是通过自己任期内有达成共识的日志后，更新自己的commitIndex，从而使之前已经达成共识的提案提交。

1. `AppendEntries()` 

这个函数的主要功能是处理Follower接收到leader日志RPC后如何处理。主要关注日志处理的部分。

在任何时候收到请求都需要先比较任期。只有任期相同时才会进行日志的处理并且重启定时器，确认这是一个有效的心跳包。

```cpp
if (args->term() < m_currentTerm) {
    reply->set_success(false);
    reply->set_term(m_currentTerm);
    reply->set_updatenextindex(-100);  // 论文中：让领导人可以及时更新自己
    return;  // 注意从过期的领导人收到消息不要重设定时器
  }
  if (args->term() > m_currentTerm) {
    // 三变 ,防止遗漏，无论什么时候都是三变
    m_status = Follower;
    m_currentTerm = args->term();
    m_votedFor = -1;  // 这里设置成-1有意义，如果突然宕机然后上线理论上是可以投票的
   
  }
  m_status = Follower;  // 这里是有必要的，因为如果candidate收到同一个term的leader的AE，需要变成follower
  // term相等
  m_lastResetElectionTime = now();
```

日志处理部分：三种情况

```cpp
if (args->prevlogindex() > getLastLogIndex()) {
    reply->set_success(false);
    reply->set_term(m_currentTerm);
    reply->set_updatenextindex(getLastLogIndex() + 1);
    return;
  } else if (args->prevlogindex() < m_lastSnapshotIncludeIndex) {
    // 如果prevlogIndex还没有更上快照
    reply->set_success(false);
    reply->set_term(m_currentTerm);
    reply->set_updatenextindex(m_lastSnapshotIncludeIndex +1);
  }

  if (matchLog(args->prevlogindex(), args->prevlogterm())) {
    for (int i = 0; i < args->entries_size(); i++) {
      auto log = args->entries(i);
      if (log.logindex() > getLastLogIndex()) {
        m_logs.push_back(log);
      } else {
        if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() == log.logterm() &&
            m_logs[getSlicesIndexFromLogIndex(log.logindex())].command() != log.command()) {
          // 相同位置的log ，其logTerm相等，但是命令却不相同，不符合raft的前向匹配，异常了！
          myAssert(false, format("[func-AppendEntries-rf{%d}] 两节点logIndex{%d}和term{%d}相同，但是其command{%d:%d}   "
                                 " {%d:%d}却不同！！\n",
                                 m_me, log.logindex(), log.logterm(), m_me,
                                 m_logs[getSlicesIndexFromLogIndex(log.logindex())].command(), args->leaderid(),
                                 log.command()));
        }
        //这个意思是在m_logs中找到日志号为logIndex的日志，比较这个日志与当前日志任期的问题，如果任期不匹配就更新日志
        if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() != log.logterm()) {
          // 不匹配就更新
          m_logs[getSlicesIndexFromLogIndex(log.logindex())] = log;
        }
		    if (args->leadercommit() > m_commitIndex) {
					  m_commitIndex = std::min(args->leadercommit(), getLastLogIndex());
		    }
		    // 领导会一次发送完所有的日志
		    reply->set_success(true);
		    reply->set_term(m_currentTerm);
		    return;
		  }
      }
    }
  }
```

如果日志不匹配：则需要向leader表示自己当前需要的匹配的日志号，通常是一个一个向前尝试。
同一 Term 内的日志是连续的：如果`prevLogIndex`的 Term 不匹配，那么该 Term 内的所有日志都不匹配，直接回退到该 Term 的第一个位置 + 1。

```cpp
 else {
    reply->set_updatenextindex(args->prevlogindex());
    for (int index = args->prevlogindex(); index >= m_lastSnapshotIncludeIndex; --index) {
      if (getLogTermFromLogIndex(index) != getLogTermFromLogIndex(args->prevlogindex())) {
        reply->set_updatenextindex(index + 1);
        break;
      }
    }
    reply->set_success(false);
    reply->set_term(m_currentTerm);

    return;
```

# 跳表部分

跳表是一种多层级的有序链表，是一种概率型的数据结构，可以实现平均 `O(logn)` ， 最坏 `O(n)` 的查询速度

实现思路：

1. 底层（Level 0） ：包含了所有节点的完整有序链表
2. 上层（Level 1/2/…) ： 索引层，越上层索引的节点越少
3. 查询时先从最上层索引定位到目标范围，逐层的下降到最底层，最终找到目标节点

## 并发控制

当前项目使用了读写锁，对于读操作，使用读锁，对于插入以及删除操作使用独占锁。

### 读写锁

相比互斥锁,读写锁允许更高的并行性。互斥量要么锁住状态,要么不加锁,而且一次只有一个线程可以加锁。读写锁可以有三种状态:

- 读模式加锁状态;
- 写模式加锁状态;
- 不加锁状态。

只有一个线程可以占有写模式的读写锁,但是可以有多个线程占有读模式的读写锁。

**读写锁也叫做"共享-独占锁",当读写锁以读模式锁住时,它是以共享模式锁住的;当它以写模式锁住时,它是以独占模式锁住的。**

1. 当锁处于写加锁状态时,在其解锁之前,所有尝试对其加锁的线程都会被阻塞;
2. 当锁处于读加锁状态时,所有试图以读模式对其加锁的线程都可以得到访问权,但是如果想以写模式对其加锁,线程将阻塞。这样也有问题,如果读者很多,那么写者将会长时间等待。如果有线程尝试以写模式加锁,那么后续的读线程将会被阻塞,这样可以避免锁长期被读者占有

### `shared_mutex`

C++17起。 `shared_mutex` 类是一个同步原语，可用于保护共享数据不被多个线程同时访问。与便于独占访问的其他互斥类型不同， `shared_mutex` 拥有二个访问级别：

- 共享 - 多个线程能共享同一互斥的所有权；
- 独占性 - 仅一个线程能占有互斥。
1. 若一个线程已经通过lock或try_lock获取独占锁（写锁），则无其他线程能获取该锁（包括共享的）。尝试获得读锁的线程也会被阻塞。
2. 仅当任何线程均未获取独占性锁时，共享锁（读锁）才能被多个线程获取（通过 lock_shared 、try_lock_shared ）。
3. 在一个线程内，同一时刻只能获取一个锁（共享或独占性）。

[面试问题](https://www.notion.so/31f6d0b6f53f8075be4cd6cf444be7c2?pvs=21)
