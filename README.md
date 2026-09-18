# AtlasDB

一个用 **C++17 从零实现的 LSM-Tree 键值存储引擎**。从跳表内存表到磁盘 SST、从分层压缩到快照读与范围查询，从崩溃持久化 WAL 到 manifest 原子提交，完整走通了现代 LSM 存储引擎的核心链路，并通过 17 个测试套件、200+ 用例（含故障注入与崩溃恢复）的回归验证。

> 当前版本：**V1.0**（嵌入式 KV 库）。WAL 逐条持久化、seq/SST/manifest 原子发布、compaction 原子提交均已闭环——**任意时刻断电，重启不丢已确认写入**。

## 特性

- **写入与删除** —— 内存表(SkipList,active + frozen 双队列,字节记账自动冻结)→ L0 SST → 分层 LSM,后台自动压缩(级联式,压完再比)；**分段 WAL** 与 memtable 分代绑定，逐条 append 即 fsync
- **崩溃持久化** —— seq 水位 / SST / manifest 一律"临时文件 + fsync + rename + 目录 fsync"原子发布；WAL 段创建/删除同步目录；compaction 换版经 manifest 声明活表集合原子提交（发布先于删除），崩溃后重启只加载被声明的完整旧版或新版，孤儿与"复活"文件天然免疫
- **点查 / 批量点查** —— memtable → L0(新→旧)→ L1+(层内二分),墓碑全链路短路
- **快照读(MVCC 风格)** —— 每条记录携带全局单调递增的 commit sequence,`put` 返回本次写入的 seq,可直接作为"读己之写"的快照点
- **全库扫描与范围查询** —— `range(lo, hi)` 半开区间查询,谓词下推到每层(SST 块二分、层内 SST 二分),只物化命中部分
- **重启恢复** —— manifest 声明有效 SST 集合（缺失时自动从目录引导迁移），严格校验：坏清单行 / 引用文件缺失 / 文件损坏一律拒绝打开；WAL 分段回放；seq 水位保证跨重启取号不回退
- **优雅关闭** —— 原子写门禁(关闭瞬间无掉队写)+ drain 全量刷盘 + 析构兜底
- **工程细节** —— RAII 文件句柄(move-only 单所有者)、SST 块级 CRC、坏块容忍、64KB value 入口拒绝、写错误锁存(fail-stop)

## 快速开始

### 依赖

- C++17 编译器(g++ ≥ 9 / clang++ ≥ 10)
- CMake ≥ 3.21（使用仓库内的跨平台 presets）
- zlib

### 构建与运行测试

```bash
cmake --preset dev
cmake --build --preset dev -j
ctest --preset dev
```

`CMakePresets.json` 只使用 `${sourceDir}`，不会记录某台机器上的项目绝对路径。
在 WSL 或 macOS 首次打开项目时重新执行配置即可；不要复制或复用其他机器生成的
`build/`、`build-*`、`test/build*` 目录。

VS Code 建议安装仓库推荐的 CMake Tools 与 C/C++ 扩展。打开仓库根目录后选择
`dev` preset，扩展会从 CMake 获取编译器、头文件目录和编译参数，无需手写
`includePath` 或编译器绝对路径。如果使用 clangd，仓库内的 `.clangd` 也会读取
同一份 `build/dev/compile_commands.json`。

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

	db.flush();	 // 手动刷成 SST(写已由 WAL 持久化;这决定何时进 SST/触发压缩)
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
| `WAL` | 分段预写日志:一段 ↔ 一张 memtable 分代,逐条 append+fsync,回放幂等 |
| `LSMEngine` | 引擎内核:读写路径、压缩、恢复、seq 取号与水位、写门禁 |
| `manifest` | 活表集合声明:flush/compact 换版时全量快照原子发布,恢复只信声明(见设计取舍) |
| `LSM` | 对外门面:窄接口,把内部概念(裸谓词、墓碑、层结构)挡在墙外 |

### 一些设计取舍

- **统一的"货币"**:迭代器世界流通 `(enc_key, value)` 对——user_key + 7B 大端 seq + 1B 类型编码在 key 里,跨任何迭代器边界无损,使跨源归并去重可以由数据本身的序决定
- **范围查询走单堆物化**:L1+ 的 ConcatIterator 表达不了"表内中途终点",而谓词定位后的命中数据本来就要逐条产出——物化进一个堆,墓碑压制、快照过滤、同 key 取最新一并收口
- **seq 水位文件**:每次 flush 前先持久化已分配的最大 seq,再落 SST——保证"凡落盘数据的 seq 必 ≤ 已持久水位",重启后计数器永不回退
- **manifest:声明优于推断**:恢复不靠"目录里有什么"猜,靠一份原子发布的小文件声明"哪些 SST 构成当前版本";compaction 换版四拍(新表 durable → 发布新声明 → 内存换表 → 删旧文件),崩溃落在任何缝隙,重启都拿到完整的旧版或新版,删一半/复活的旧表是清单外孤儿
- **move-only `FileObj`**:拷贝 `= delete`、移动接管 fd、析构守卫关闭——用类型系统封死描述符的共享所有权

## 项目结构

```
AtlasDB/
├── include/   # 头文件(存储格式、迭代器、引擎、门面)
├── src/       # 实现
└── test/      # 17 个测试套件(CMake + ctest,含故障注入)
```

## 测试

17 个测试套件覆盖每一层:SkipList / MemTable / Block / BlockMeta / SST / FileObj / 引擎读写路径 / 压缩(含级联)/ 全库扫描 / 谓词范围查询 / 门面(含跨实例水位续接)/ **WAL(写入、恢复、故障注入)**。故障注入覆盖:WAL 单次 write 错误 / 短写 / EINTR / 零进展 / fsync 错误(链接器 `--wrap=write/fsync`,按 fd 路径限定)、发布点定点断电(fork + 定点 `_exit`)、压缩期 SST 删除失败、manifest 坏行/缺表/坏表拒绝打开、复活 SST 屏蔽。

```bash
ctest --preset dev
# 100% tests passed, 0 tests failed out of 17
```

## 已知限制(V1.0)

以下均为知情取舍而非待修缺陷:

- 单条 value 上限 **64KB**(块格式以 16 位长度编码,超限在入口被拒绝而非截断)
- 引擎并发为"多读单写"安全,尚未做系统化并发压测;不支持多进程共享同一数据目录
- `put`/`batch` 返回 `nullopt` 不区分拒绝原因(已关闭 / WAL 错误 / 入参超限);batch 无跨条原子性
- 压缩在写锁内同步执行,压缩期间写者被阻塞(吞吐换正确性,缩锁/后台压缩属后续优化)
- WAL 回放依赖"WAL ↔ memtable ↔ SST 同字节序列"——内容完全相同的重复行会被 keep_all 压缩保留(查询路径不可见;清理属墓碑回收一批)
- 极端编号边界(段号逼近 2^64,实际不可达)下最后一次运行存在拒绝打开的不对称;非空但零完整记录的 WAL 段保守保留不删(防解析器误删数据,残留有界)

## 路线图

- [x] **WAL + 全链路 fsync + manifest 原子提交** → V1.0(任意时刻崩溃不丢已确认写入)
- [ ] MVCC / 事务(begin / commit / 快照隔离)
- [ ] RESP 协议服务器形态
- [ ] 底层墓碑回收(顺带同键同值重复行清理)、大 value 分离(Wisckey 方向)、压缩缩锁/后台化

## 致谢

本项目在学习 LSM-Tree 的过程中参考了 [tiny-lsm](https://github.com/lispking/tiny-lsm) 的教程思路;存储格式与多处核心设计(迭代器货币、单堆物化范围查询、水位协议、门面语义等)为独立实现。
