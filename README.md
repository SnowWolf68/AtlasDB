# AtlasDB

一个用 **C++17 从零实现的 LSM-Tree 键值存储引擎**。从跳表内存表到磁盘 SST、从分层压缩到快照读与范围查询,完整走通了现代 LSM 存储引擎的核心链路,并通过 16 个测试套件、200+ 用例的回归验证。

> 当前版本:**V1.0-alpha**(嵌入式 KV 库,可对外使用;崩溃持久化 WAL 属于下一阶段,见[路线图](#路线图))。

## 特性

- **写入与删除** —— 内存表(SkipList,active + frozen 双队列,字节记账自动冻结)→ L0 SST → 分层 LSM,后台自动压缩(级联式,压完再比)
- **点查 / 批量点查** —— memtable → L0(新→旧)→ L1+(层内二分),墓碑全链路短路
- **快照读(MVCC 风格)** —— 每条记录携带全局单调递增的 commit sequence,`put` 返回本次写入的 seq,可直接作为"读己之写"的快照点
- **全库扫描与范围查询** —— `range(lo, hi)` 半开区间查询,谓词下推到每层(SST 块二分、层内 SST 二分),只物化命中部分
- **重启恢复** —— 扫描数据目录重建引擎索引;seq 水位文件保证跨重启取号不回退(新写永不与历史数据撞版本号)
- **优雅关闭** —— 原子写门禁(关闭瞬间无掉队写)+ drain 全量刷盘 + 析构兜底
- **工程细节** —— RAII 文件句柄(move-only 单所有者)、SST 块级 CRC、坏块容忍、64KB value 入口拒绝

## 快速开始

### 依赖

- C++17 编译器(g++ ≥ 9 / clang++ ≥ 10)
- CMake ≥ 3.16
- zlib

### 构建与运行测试

```bash
cd test
cmake -B build-check .
cmake --build build-check -j
ctest --test-dir build-check --output-on-failure
```

### 作为库使用

AtlasDB 的对外接口是一个 `LSM` 门面类,编译时把 `src/` 与 `include/` 一起带上即可:

```cpp
#include "lsm.h"

int main() {
	LSM db("/tmp/atlas-demo");

	// 写入:返回本次分配的 seq(可作读己之写的快照点)
	auto seq = db.put("name", "atlas");
	auto res = db.get("name");						// 命中 -> "atlas"

	db.put("k0100", "v0");
	db.put("k0101", "v1");
	db.remove("name");

	// 范围查询:[lo, hi) 半开区间
	HeapIterator it = db.range("k0100", "k0200");
	while (it.is_valid()) {
		auto [enc_key, value] = *it;
		++it;
	}

	// 全库扫描
	for (auto i = db.begin(); i != db.end(); ++i) { /* ... */ }

	db.flush();	 // 手动落盘(重要写之后的持久化手段)
	db.close();	 // 优雅关闭;析构亦会自动兜底
}
```

```bash
g++ -std=c++17 -Iinclude demo.cpp src/*.cpp -lz -o demo
```

## 架构

```
            ┌────────────────────────────┐
   写入 ──▶ │  MemTable (SkipList)        │  active 表写满自动冻结
            │  active + frozen 队列       │
            └──────────┬─────────────────┘
                       │ flush(水位先行)
                       ▼
            ┌────────────────────────────┐
            │  L0  (SST, 表间可重叠)      │  表数到阈值触发压缩
            └──────────┬─────────────────┘
                       │ compact(L0→L1)
                       ▼
            ┌────────────────────────────┐
            │  L1..Ln (SST, 层内有序不重叠)│  目标容量逐层放大,级联压缩
            └────────────────────────────┘
```

| 组件 | 说明 |
|---|---|
| `SkipList` | 内存有序表,迭代器 + 谓词定位,字节记账 |
| `MemTable` | active/frozen 双队列,刷盘、快照读、批量接口 |
| `SST` | 只读磁盘表:Block 数据 + 元信息区 + footer(含 magic),块级 CRC |
| `HeapIterator` / `ConcatIterator` / `TwoMergeIterator` / `LSMEngineIterator` | 统一 `BaseIterator` 多态体系:堆归并、层内串联、双路归并、全库多路归并 |
| `LSMEngine` | 引擎内核:读写路径、压缩、恢复、seq 取号与水位、写门禁 |
| `LSM` | 对外门面:窄接口,把内部概念(裸谓词、墓碑、层结构)挡在墙外 |

### 一些设计取舍

- **统一的"货币"**:迭代器世界流通 `(enc_key, value)` 对——user_key + 7B 大端 seq + 1B 类型编码在 key 里,跨任何迭代器边界无损,使跨源归并去重可以由数据本身的序决定
- **范围查询走单堆物化**:L1+ 的 ConcatIterator 表达不了"表内中途终点",而谓词定位后的命中数据本来就要逐条产出——物化进一个堆,墓碑压制、快照过滤、同 key 取最新一并收口
- **seq 水位文件**:每次 flush 前先持久化已分配的最大 seq,再落 SST——保证"凡落盘数据的 seq 必 ≤ 已持久水位",重启后计数器永不回退
- **move-only `FileObj`**:拷贝 `= delete`、移动接管 fd、析构守卫关闭——用类型系统封死描述符的共享所有权

## 项目结构

```
AtlasDB/
├── include/   # 头文件(存储格式、迭代器、引擎、门面)
├── src/       # 实现
└── test/      # 16 个测试套件(CMake + ctest)
```

## 测试

16 个测试套件覆盖每一层:SkipList / MemTable / Block / BlockMeta / SST / FileObj / 引擎读写路径 / 压缩(含级联)/ 全库扫描 / 谓词范围查询 / 门面(含跨实例水位续接)。

```bash
ctest --test-dir test/build-check
# 100% tests passed, 0 tests failed out of 16
```

## 已知限制(V1.0-alpha)

- **崩溃持久化尚未实现(WAL 属下一阶段)**:进程崩溃会丢失未刷盘的 memtable 数据;优雅关闭(`close()` / 析构)不丢
- 单条 value 上限 **64KB**(块格式以 16 位长度编码,超限在入口被拒绝而非截断)
- 水位文件暂未使用 rename 原子替换与 fsync(断电撕裂窗口为已知的低概率取舍)
- 引擎并发为"多读单写"安全,尚未做并发压测

## 路线图

- [ ] **WAL + 全链路 fsync** → V1.0 正式(崩溃不丢已确认写入)
- [ ] MVCC / 事务(begin / commit / 快照隔离)
- [ ] RESP 协议服务器形态
- [ ] 底层墓碑回收、大 value 分离(Wisckey 方向)

## 致谢

本项目在学习 LSM-Tree 的过程中参考了 [tiny-lsm](https://github.com/lispking/tiny-lsm) 的教程思路;存储格式与多处核心设计(迭代器货币、单堆物化范围查询、水位协议、门面语义等)为独立实现。
