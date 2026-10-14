# AtlasDB

一个用 **C++17 从零实现的持久化 KV 数据库**。采用 LSM-Tree 存储结构,实现 WAL 恢复、manifest 原子发布、MVCC 事务和 Reactor 网络服务,提供嵌入式 C++ 接口与 RESP 网络访问。当前在 Linux 环境通过 33 个测试套件,覆盖存储、事务、协议、网络生命周期及故障注入。

> 当前版本:**V1.2**(嵌入式 KV 库 + RESP 服务器)。网络入口支持 PING、SET、GET 和单键 DEL;事务和扫描通过 C++ 接口使用。验证范围与已知限制见下文。

## 特性

- **写入与删除** —— 内存表(SkipList,active + frozen 双队列,字节记账自动冻结)→ L0 SST → 分层 LSM,达到阈值后同步触发级联压缩;**分段 WAL** 与 memtable 分代绑定,逐条 append 即 fsync
- **事务(MVCC)** —— `begin_transaction(IsolationLevel)` 开启事务:RC(每条读语句看最新已提交)/ RR(begin 时固定快照 + 提交时写冲突检测,fail-fast 返回 Conflict);私有写集、读己之写、`abort` 随时弃单;**整笔事务共用一个提交 seq、WAL 单条事务记录,崩溃恢复 all-or-nothing**
- **已发布水位读写语义** —— 引擎的提交序号是"已发布水位":写者在锁内整笔应用完成后才推进,读者一律以水位为可见上界——未提交/提交中的半笔事务对任何读者(点查/批量点查/扫描/事务快照)不可见
- **点查 / 批量点查** —— memtable → L0(新→旧)→ L1+(层内二分),墓碑全链路短路;一次 `get_batch` 单一视图(一次取样)
- **全库扫描与范围查询** —— `range(lo, hi)` 半开区间查询,谓词下推到每层(SST 块二分、层内 SST 二分),只物化命中部分
- **重启恢复** —— manifest 声明有效 SST 集合(缺失时自动从目录引导迁移);解析失败、引用 SST 缺失或 SST::open 失败时拒绝打开,数据块在读取时校验 CRC;WAL 分段回放(按事务记录,CRC 通过整笔回放、残尾截断);水位取 `max(seq.txt, WAL 最大 seq)` 续接
- **优雅关闭** —— 原子写门禁(关闭瞬间无掉队写)+ drain 全量刷盘 + 析构兜底
- **RESP 网络服务** —— Linux epoll Reactor、工作线程池、TCP 连接管理、分片与管线请求解析;主程序通过 signalfd 接收 SIGINT,等待服务器停止完成后退出事件循环
- **工程细节** —— RAII 文件句柄(move-only 单所有者)、SST 块级 CRC、坏块容忍、key/value 长度检查、写错误锁存(fail-stop)、文件读取堆缓冲(大 WAL 恢复不炸栈)

## 快速开始

### 依赖

- Linux 环境(包括 WSL2 Linux);当前网络层使用 epoll、eventfd、timerfd 和 signalfd
- 支持 C++17 的编译器(本次验证使用 GCC 11.4.0)
- CMake ≥ 3.21(使用仓库内的 presets)
- zlib
- redis-cli(可选,用于服务器联调)

### 构建与运行测试

```bash
cmake --preset dev
cmake --build --preset dev -j
ctest --preset dev
```

`CMakePresets.json` 只使用 `${sourceDir}`,不会记录某台机器上的项目绝对路径。
在新的 Linux/WSL2 环境首次打开项目时重新执行配置即可;不要复制或复用其他机器生成的
`build/`、`build-*`、`test/build*` 目录。

使用 VS Code 时可安装 CMake Tools 与 C/C++ 扩展。打开仓库根目录后选择
`dev` preset,扩展会从 CMake 获取编译器、头文件目录和编译参数,无需手写
`includePath` 或编译器绝对路径。如果使用 clangd,仓库内的 `.clangd` 也会读取
同一份 `build/dev/compile_commands.json`。

### 启动服务器

在仓库根目录启动:

```bash
./build/dev/AtlasDB
```

当前入口监听 `127.0.0.1:6379`,启动 4 个工作线程,数据保存到启动目录下的 `./data/`。这些参数暂未提供命令行配置;本地 `data/` 已加入 Git 忽略规则。

在另一个终端使用 redis-cli:

```bash
redis-cli -h 127.0.0.1 -p 6379 PING
redis-cli -h 127.0.0.1 -p 6379 SET hello atlas
redis-cli -h 127.0.0.1 -p 6379 GET hello
redis-cli -h 127.0.0.1 -p 6379 DEL hello
```

| 命令 | 行为 |
|---|---|
| `PING [message]` | 无参数返回 PONG,有一个参数时回显 |
| `SET key value` | 写入字符串,成功返回 OK |
| `GET key` | 返回字符串;不存在时返回 null bulk string,与空字符串区分 |
| `DEL key` | 删除单个 key,返回 1 或 0 |

命令名不区分大小写。参数错误和未知命令回复错误并保持连接;协议解析错误关闭对应连接。某些 redis-cli 版本会在连接初始化时发送 `COMMAND` 等探测命令,当前未实现这些命令,可能看到不支持命令的响应或日志。

在服务器终端按 **Ctrl+C** 发起正常停服。保持相同启动目录重新运行程序,可以继续读取此前写入的数据。

### 作为库使用

AtlasDB 的对外接口是一个 `LSM` 门面类。完成上述构建后,可链接生成的 `atlasdb` 静态库;下面的示例保存为 `demo.cpp`:

```cpp
#include "lsm.h"
#include "TranContext.h"	 // 事务上下文的完整定义(门面头里只有前置声明)

int main() {
	LSM db("/tmp/atlas-demo");

	// 写入:返回本次分配的 seq
	auto seq = db.put("name", "atlas");
	auto res = db.get("name");						// 命中 -> "atlas"

	db.put("k0100", "v0");
	db.put("k0101", "v1");
	db.remove("name");

	// 事务:RR 固定快照 + 整笔原子提交
	auto tx = db.begin_transaction(IsolationLevel::ReadRepeated);
	tx->put("k0100", "v9");							// 私有写集,读己之写
	tx->remove("k0101");
	auto commit = tx->commit();						// Committed / Conflict / Closed
	if (commit.status == CommitResult::Committed) {
		auto tran_seq = commit.commit_seq.value();	// 整笔共用的提交 seq
	}

	// 范围查询:[lo, hi) 半开区间
	HeapIterator it = db.range("k0100", "k0200");
	while (it.is_valid()) {
		auto [enc_key, value] = *it;
		++it;
	}

	// 全库扫描
	for (auto i = db.begin(); i != db.end(); ++i) { /* ... */ }

	db.flush();	 // 手动刷成 SST(写已由 WAL 持久化;这决定何时进 SST/触发压缩)
	db.close();	 // 优雅关闭;析构亦会自动兜底
}
```

```bash
g++ -std=c++17 -Iinclude demo.cpp build/dev/libatlasdb.a -pthread -lz -o demo
./demo
```

## 架构

访问入口与写入路径如下。事务先在 `TranContext` 中暂存写集,提交时进入引擎;普通写入直接进入引擎。读请求查询 MemTable 和 SST,不经过 WAL。

```
RESP 客户端 ──▶ AtlasServer ──▶ LSM 门面 ◀── C++ 应用
                                  │
                    ┌─────────────┴─────────────┐
                    │                           │
                 普通写入              TranContext 事务写集
                    │                           │ commit
                    └─────────────┬─────────────┘
                                  ▼
                              LSMEngine
                                  │
                           WAL append + fsync
                                  │
                                  ▼
                       MemTable(active + frozen)
                                  │ flush
                                  ▼
                         L0 SST(表间可重叠)
                                  │ compact
                                  ▼
                    L1..Ln SST(层内有序不重叠)
```

| 组件 | 说明 |
|---|---|
| `SkipList` | 内存有序表,迭代器 + 谓词定位,字节记账 |
| `MemTable` | active/frozen 双队列,刷盘、快照读、批量接口 |
| `SST` | 只读磁盘表:Block 数据 + 元信息区 + footer(含 magic),块级 CRC |
| `HeapIterator` / `ConcatIterator` / `TwoMergeIterator` / `LSMEngineIterator` | 统一 `BaseIterator` 多态体系:堆归并、层内串联、双路归并、全库多路归并 |
| `WAL` | 分段预写日志:一段 ↔ 一张 memtable 分代;**只含事务记录(type=0x02,整笔一条 + CRC)**,逐条 append+fsync,回放幂等、残尾截断 |
| `LSMEngine` | 引擎内核:读写路径、压缩、恢复、**已发布水位**(写者整笔应用后才推进)、写门禁 |
| `TranContext` | 事务上下文:私有写集、快照 seq、RC/RR 隔离、Active/Committed/Aborted 状态机;commit 走引擎整笔提交 |
| `manifest` | 活表集合声明:flush/compact 换版时全量快照原子发布,恢复只信声明(见设计取舍) |
| `LSM` | 对外门面:窄接口,把内部概念(裸谓词、墓碑、层结构)挡在墙外 |
| `EventLoop` / `Channel` / `EventLoopThreadPool` | epoll 事件循环、fd 事件分发和工作线程池 |
| `TcpServer` / `TcpConnection` | 接收连接、所属 loop 内处理读写、异步停服与连接回收 |
| `Resp` / `AtlasServer` | RESP 值解析与编码、每连接独立解析状态、命令分发与存储接入 |

### 一些设计取舍

- **统一的"货币"**:迭代器世界流通 `(enc_key, value)` 对——user_key + 7B 大端 seq + 1B 类型编码在 key 里,跨任何迭代器边界无损,使跨源归并去重可以由数据本身的序决定
- **已发布水位(方案 A:提交时取号)**:seq 在提交临界区内分配,"分配序 = 提交序";计数器语义是**已发布水位**——写者锁内 `seq = 水位+1` 为局部号,整笔 WAL + memtable 应用完成后才推进一次。读者(含事务快照)一律以水位为上界,半笔事务天然不可见;空库水位从 0 起,首笔提交 seq = 1
- **整笔一个 seq**:事务的原子可见性由 seq 界线免费保证——一笔事务的所有 key 共享同一个提交 seq,WAL 中是单条记录,恢复按整笔回放或整笔丢弃
- **隔离菜单只做 RC + RR**:RU 不做(未提交数据不进存储,无从实现);SER 不做(需要读集追踪);RR = 固定快照 + 提交时写写冲突检测(first-committer-wins,先成功提交者保留写入,后提交者发现冲突即返回 Conflict,不等待不重试)
- **范围查询走单堆物化**:L1+ 的 ConcatIterator 表达不了"表内中途终点",而谓词定位后的命中数据本来就要逐条产出——物化进一个堆,墓碑压制、快照过滤、同 key 取最新一并收口
- **manifest:声明优于推断**:恢复不靠"目录里有什么"猜,靠一份原子发布的小文件声明"哪些 SST 构成当前版本";compaction 换版四拍(新表 durable → 发布新声明 → 内存换表 → 删旧文件),崩溃落在任何缝隙,重启都拿到完整的旧版或新版,删一半/复活的旧表是清单外孤儿
- **move-only `FileObj`**:拷贝 `= delete`、移动接管 fd、析构守卫关闭——用类型系统封死描述符的共享所有权

## 项目结构

```
AtlasDB/
├── include/   # 存储、事务、协议与服务器头文件;net/ 为网络基础层
├── src/       # 实现;main.cpp 为服务器入口,net/ 为 Reactor/TCP 实现
└── test/      # 33 个测试套件(CMake + ctest,含故障注入与服务器集成测试)
```

## 测试

33 个测试套件覆盖 SkipList、MemTable、Block、SST、FileObj、引擎读写、压缩、扫描、范围查询、门面、WAL 和事务,以及 RESP、地址/Socket、事件循环、定时器、工作线程池、TCP 连接/服务器和 AtlasServer。服务器集成测试通过真实本地 TCP 验证命令、空值与二进制值、分片、管线、连接隔离、多客户端、异常请求、停服和数据恢复。

2026-10-14 在 Linux 环境完整构建成功,**33/33 个测试套件通过**。本轮增加回归覆盖:共享 fd 定位读取交错、pread 短读/EINTR、前导 SST/Block 被快照过滤、Block 精确二分边界与非首条取值、扫描迭代器重复加锁、RESP 非法成员类型及嵌套数组分片。故障注入还覆盖 WAL write/fsync 错误、发布点进程退出、manifest 异常和压缩清扫失败。

`fork` + `_exit` 不会清除内核缓存,目录同步探针验证的是调用及错误传播;这些测试不等同于真实断电验证。持久化依赖底层文件系统与存储设备正确执行 `fsync`/`rename`,当前未进行真实断电试验。

```bash
ctest --preset dev
# 100% tests passed, 0 tests failed out of 33
```

## 已知限制(V1.2)

当前使用边界与已知暂缓项:

- 网络入口目前仅支持上述四种命令,不提供完整 Redis 命令集、认证、TTL 或网络事务;MVCC 事务通过嵌入式接口使用
- RESP 已支持嵌套数组解码,编码仍不支持嵌套数组,null 值只在服务器 GET 响应中处理;最大嵌套深度限制暂缓。半关闭、输出排空和高水位/资源上限等网络扩展未纳入本版
- 单条 value 上限 **65535 字节**(块格式以 16 位长度编码,超限在入口被拒绝而非截断);**单笔事务的写集驻留内存、总大小无上限**(大事务 = 大内存,入口只限单条 value)
- **RR 是"快照隔离 + 写冲突检测"**:不追踪读集,不能防御写偏斜,不是可串行化(知情取舍);冲突即中止返回 Conflict,无等待/重试
- 引擎写路径通过锁串行化,已有部分并发回归测试,尚未做系统化并发压测;不支持多进程共享同一数据目录
- `put`/`batch` 返回 `nullopt` 不区分拒绝原因(已关闭 / WAL 错误 / 入参超限);**裸 `put_batch`/`remove_batch` 仍是逐条独立提交、无跨条原子性——需要原子批量请使用事务**(事务本身就是原子批量)
- 压缩在写锁内同步执行,压缩期间写者被阻塞(吞吐换正确性,缩锁/后台压缩属后续优化)
- WAL 回放依赖"WAL ↔ memtable ↔ SST 同字节序列"——内容完全相同的重复行会被 keep_all 压缩保留(查询路径不可见;清理属墓碑回收一批)
- 极端编号边界(段号逼近 2^64,实际不可达)下最后一次运行存在拒绝打开的不对称;非空但零完整记录的 WAL 段保守保留不删(防解析器误删数据,残留有界)
- 已有 SST 时,`seq.txt` 缺失不会从 SST 重建最大 seq;水位读取尚未校验完整 8 字节,这些异常元数据场景仍待加固
- `close()` 的 drain 循环尚未增加刷盘无进展检测,异常关闭策略仍为后续加固项
- **`seq.txt` 语义随 V1.1 变更**:现保存**已发布水位**(V1.0 保存"下一个待分配号")。旧数据目录无需迁移即可打开——旧值按新语义解释会多跳一个序号(方向安全,不回退不复用)

## 路线图

- [x] **WAL + 全链路 fsync + manifest 原子提交** → V1.0(通过当前进程崩溃恢复与故障注入回归)
- [x] **MVCC / 事务(begin / commit / RC+RR 快照隔离 + 冲突检测)** → V1.1(tran_test + transaction_review_test 回归)
- [x] **RESP 协议服务器形态** → V1.2(PING/SET/GET/单键 DEL、线程池、SIGINT 停服和集成测试)
- [ ] 可复现性能基线与按测量结果推进的优化
- [ ] 底层墓碑回收(顺带同键同值重复行清理)、大 value 分离(Wisckey 方向)、压缩缩锁/后台化

## 致谢

本项目在学习 LSM-Tree 的过程中参考了 [tiny-lsm](https://github.com/Vanilla-Beauty/tiny-lsm) 的教程思路;存储格式与多处核心设计(迭代器货币、单堆物化范围查询、水位协议、门面语义等)为独立实现。
