# 高性能低延迟撮合引擎

基于 **C++17 / Linux** 实现的生产级低延迟订单撮合引擎。本项目展示了高频交易（HFT）系统的核心架构设计，包括无锁并发模式、UDP 网络层、内存池分配器、以及系统级性能优化技术。

## 核心特性

| 特性 | 说明 |
|------|------|
| **订单类型** | 限价单（Limit）、市价单（Market）、撤单（Cancel） |
| **撮合算法** | 价格优先 / 时间优先（Price-Time Priority，同价位 FIFO） |
| **无锁并发** | SPSC 无锁环形队列 + 无锁内存池 + CAS 自由链表 |
| **网络层** | UDP 二进制协议接收订单，独立接收线程，零拷贝解析 |
| **无锁订单簿** | 单线程独占访问，完全移除 mutex 锁竞争 |
| **性能优化** | Cache Line 对齐、False Sharing 预防、`_mm_pause()` 自旋退避、`-O3 -march=native` |
| **基准测试** | Warmup + Run 两阶段压测，Lock-Free Ring Buffer 延迟采集（无 mutex 污染） |

## 系统架构

```
                         ┌──────────────────────────────────────────────────────┐
                         │                  外部世界                            │
                         │                                                    │
                         │  ┌──────────┐  UDP/WireOrder   ┌────────────────┐  │
                         │  │UDP Client│ ──(34B binary)──▶ │                │  │
                         │  │(测试工具) │                   │    网络层       │  │
                         │  └──────────┘                   │                │  │
                         │                                  │  UDPServer     │  │
                         │  ┌──────────┐  内存Order       │  recv_loop()   │  │
                         │  │Producer  │ ───────────────▶ │                │  │
                         │  │Generator │  (direct模式)     └───────┬────────┘  │
                         │  └──────────┘                          │           │
                         └────────────────────────────────────────┼───────────┘
                                                          LockFreeQueue
                                                          (SPSC, 无锁)
                                                          capacity=65536
                                                                  │
                                                         ┌────────▼────────┐
                                                         │                 │
                                                         │  撮合引擎核心     │
                                                         │ MatchingEngine   │
                                                         │                 │
                                                         │ process_order()  │
                                                         │  ├─ slab_.alloc() │
                                                         │  ├─ match_limit_* │
                                                         │  ├─ match_market_*│
                                                         │  ├─ handle_cancel  │
                                                         │  ├─ record_trade() │
                                                         │  └─ slab_.free()   │
                                                         └────┬──────┬───────┘
                                                              │      │
                                                   ┌──────────┘      └──────────┐
                                                   │                             │
                                        ┌──────────▼────────┐      ┌──────────▼────────┐
                                        │                    │      │                    │
                                        │   SlabAllocator    │      │    OrderBook        │
                                        │                    │      │                    │
                                        │ ┌────┬────┬─────┐  │      │  PriceLadder[100K] │
                                        │ │32B │64B │128B │  │      │  + IntrusiveList   │
                                        │ └────┴────┴─────┘  │      │  (零堆分配/O(1))    │
                                        │ CAS无锁自由链表     │      │                     │
                                        │ 预分配30万块        │      │ bids_[100001]       │
                                        │ alignas(64)隔离     │      │ asks_[100001]       │
                                        └────────────────────┘      └────────────────────┘
                                                                             
                                                         ┌────────────────────┐
                                                         │   Logger 线程      │
                                                         │  (异步轮询atomic)   │
                                                         │  输出统计/状态      │
                                                         └────────────────────┘
```

### 三线程模型

| 线程 | 职责 | 关键组件 | 同步方式 |
|------|------|---------|---------|
| **Producer** | 订单生成 / UDP 接收与解析 | `OrderGenerator` 或 `UDPServer::recv_loop` | `queue.push()` — 原子 tail 更新 |
| **Consumer** | 撮合引擎核心逻辑（唯一写入者） | `MatchingEngine::process_order()` → 四种撮合函数 | `queue.pop()` — 原子 head 更新；独占 OrderBook/Slab |
| **Logger** | 异步统计与日志输出 | 轮询 `EngineStats` / `NetworkStats` atomic 变量 | `memory_order_relaxed` 读，完全不阻塞 |

### 数据流全景

```
外部订单源
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│  Layer 1: 网络层 (Network)                                      │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ UDPServer::recv_loop()                                 │   │
│  │   ├── Non-blocking UDP socket (SOCK_DGRAM | O_NONBLOCK)│   │
│  │   ├── SO_RCVBUF = 256KB                               │   │
│  │   ├── 批量解析: WireOrder[] (34B/order, #pragma pack)  │   │
│  │   └── 统计: packets / bytes / dispatched / drops/errors│   │
│  └────────────────────┬────────────────────────────────────┘   │
└───────────────────────┼─────────────────────────────────────────┘
                        │ push(Order)
                        ▼
┌─────────────────────────────────────────────────────────────────┐
│  Layer 2: 通信层 (Messaging)                                    │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ LockFreeQueue<Order> (SPSC Ring Buffer)                 │   │
│  │   ├── capacity = 65536 (2^16, 位掩码取模)              │   │
│  │   ├── head_: alignas(64) atomic (Consumer 写)          │   │
│  │   ├── tail_: alignas(64) atomic (Producer 写)          │   │
│  │   ├── push: memory_order_release (写完数据再更新tail)   │   │
│  │   └── pop:  memory_order_acquire (先读head再读数据)     │   │
│  └────────────────────┬────────────────────────────────────┘   │
└───────────────────────┼─────────────────────────────────────────┘
                        │ pop(Order)
                        ▼
┌─────────────────────────────────────────────────────────────────┐
│  Layer 3: 撮合核心 (Matching Engine) — 热路径                      │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ process_order(order)                                   │   │
│  │                                                          │   │
│  │  ① SlabAllocator::allocate()                           │   │
│  │     └── select_class(sizeof(Order)) → 128B Slab         │   │
│  │     └── CAS pop free_list → placement new Order        │   │
│  │                                                          │   │
│  │  ② OrderBook 撮合                                       │   │
│  │     ├── best_bid()/best_ask(): 扫描 PriceLadder[]       │   │
│  │     ├── list_pop_front(level): O(1) 取 maker 成交       │   │
│  │     ├── order->fill(qty): 更新 remaining/filled/status  │   │
│  │     ├── list_push_back(level): O(1) 未成交挂单          │   │
│  │     └── list_unlink(level): O(1) 撤单断链               │   │
│  │                                                          │   │
│  │  ③ SlabAllocator::deallocate(maker)                     │   │
│  │     └── order->~Order() → CAS push free_list            │   │
│  │                                                          │   │
│  │  ④ record_trade(trade) → trades_ vector                 │   │
│  │     └── stats_.total_trades.fetch_add(relaxed)         │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                  │
│  ┌──────────────────┐  ┌──────────────────────────────────┐    │
│  │  SlabAllocator   │  │  OrderBook                       │    │
│  │                  │  │                                  │    │
│  │  Slab[0] 32B     │  │  PriceLevel { head, tail, count }│    │
│  │  Slab[1] 64B     │  │       ↑                          │    │
│  │  Slab[2] 128B    │  │  Order ← intrusive linked list   │    │
│  │  (Order 用)      │  │    next ──→ next ──→ nullptr     │    │
│  │                  │  │    prev ←── prev ←── nullptr     │    │
│  │  free_list:      │  │                                  │    │
│  │  CAS lock-free   │  │  bids_[0..100000] 买盘价格阶梯   │    │
│  │  aligned(64)     │  │  asks_[0..100000] 卖盘价格阶梯   │    │
│  └──────────────────┘  │  order_index_: unordered_map      │    │
│                        └──────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
                        │
                        │ async read (relaxed atomics)
                        ▼
┌─────────────────────────────────────────────────────────────────┐
│  Layer 4: 日志层 (Logging) — 冷路径                              │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ Logger Thread                                           │   │
│  │   ├── 轮询 EngineStats: total_orders/trades/cancelled  │   │
│  │   ├── 轮询 NetworkStats: packets/drops/errors          │   │
│  │   ├── 轮询 SlabStats: allocated/available              │   │
│  │   └── std::cout 定期输出摘要 (10ms 间隔)                │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### 并发安全保证 — 竞争点消除图谱

```
                    写入者(Writer)              读取者(Reader)
                    ─────────────              ─────────────
LockFreeQueue.tail  ◀── Producer ──             Consumer (仅读)
LockFreeQueue.head       Consumer (仅读)  ───▶ Producer (仅读)

Slab.free_list[0-2]      Consumer ──             (无其他写入者)
Slab.allocated/available  Consumer ──             Logger (relaxed 读)

OrderBook.bids_/asks_     Consumer ──             (Single-Thread Ownership)
EngineStats.*             Consumer ──             Logger (relaxed 读)
trades_ vector            Consumer ──             (Consumer 后续读取)

结论: 所有共享变量要么是 SPSC 单写者，要么是 relaxed 原子读取。
      整个系统不存在 mutex / lock / condition_variable。
```

## 项目结构

```
matching_engine/
├── CMakeLists.txt                  # 构建系统 (C++17, -O3, -march=native)
├── .clangd                         # Clangd LSP 配置
├── main.cpp                        # 入口（支持 direct / udp 双模式）
├── include/
│   ├── Order.h                     # 订单/成交数据结构 (alignas(64), 含 intrusive next/prev)
│   ├── OrderBook.h                 # Price Ladder + Intrusive Linked List 订单簿
│   ├── UDPServer.h                 # UDP 服务端 + WireOrder 二进制协议 (34B)
│   ├── LockFreeQueue.h             # 无锁 SPSC 环形队列 (capacity=65536)
│   ├── SlabAllocator.h             # 三级 Slab 分配器接口 (32B/64B/128B, CAS无锁)
│   └── MatchingEngine.h            # 撮合引擎核心 + EngineStats 统计数据
├── src/
│   ├── OrderBook.cpp               # Price Ladder 实现 (best_bid/ask/list ops)
│   ├── MatchingEngine.cpp          # 价格-时间优先撮合逻辑 (4个match函数)
│   ├── UDPServer.cpp               # UDP 接收线程实现 (批量解析WireOrder)
│   └── SlabAllocator.cpp           # CAS 无锁 Slab 分配器实现 (allocate/deallocate)
├── tools/
│   └── UDPClient.cpp               # UDP 测试客户端（批量发包）
└── benchmark/
    └── Benchmark.cpp               # 性能基准测试 (Warmup+Run两阶段, chrono纳秒级)
```

## 编译构建

### 环境要求

- GCC >= 9 或 Clang >= 10（需支持 C++17）
- CMake >= 3.16
- pthreads（Linux）
- x86_64 CPU（SIMD / 对齐优化）

### 构建步骤

```bash
cd matching-engine

# 配置 Release 模式
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译（4 个目标）
make -j$(nproc)
```

### 输出产物

| 目标 | 说明 |
|------|------|
| `matching_engine` | 主程序（支持 `--mode direct` 和 `--mode udp` 双模式） |
| `benchmark` | 百万级压测工具（Warmup + Run 两阶段，Lock-Free 延迟采集） |
| `UDPClient` | UDP 测试客户端（可配置端口/订单数/批量大小） |
| `libmatching_engine_lib.a` | 静态库 |

## 使用方式

### 模式一：直连模式（内存订单生成）

```bash
# 默认 10 万订单
./build/matching_engine

# 自定义订单数
./build/matching_engine -n 1000000

# 查看帮助
./build/matching_engine --help
```

### 模式二：UDP 网络模式

```bash
# 终端 1：启动 UDP 服务端
./build/matching_engine --mode udp -p 50000

# 终端 2：发送测试订单
./build/UDPClient -p 50000 -n 1000000 -b 10

# Ctrl+C 优雅退出
```

### 基准测试

```bash
# 默认百万订单压测
./build/benchmark

# 自定义数量
./build/benchmark 5000000
```

## 运行示例

### 直连模式输出

```
╔══════════════════════════════════════════════╗
║   LOW-LATENCY MATCHING ENGINE - DIRECT MODE   ║
║     C++17 / Linux / Lock-Free / No-Mutex      ║
╚══════════════════════════════════════════════╝

Initializing matching engine...
- Allocator:  Slab (32B / 64B / 128B)
- Queue Capacity: 65,536 orders
- Matching: Price-Time Priority
- Concurrency: SPSC Lock-Free Queue
- OrderBook: Price Ladder + Intrusive Linked List

========== MATCHING ENGINE STATS ==========
Total Orders Processed: 10000
Total Trades Executed:  5619
Total Cancelled:        0
Total Rejected:         1361
Avg Latency:            94014.9 ns
Min Latency:            104 ns
Max Latency:            1095350 ns
Best Bid:               109
Best Ask:               163
Bid Depth:              11
Ask Depth:              2
Slab Allocated:         3004
Slab Available:         269996
============================================

╔══════════════════════════════════════════════╗
║              PERFORMANCE SUMMARY              ║
╠══════════════════════════════════════════════╣
║  Orders Processed:           10000               ║
║  Duration:                  943645 us             ║
║  Throughput:                 10597 ops/sec       ║
║  Avg Latency:                94014 ns           ║
╚══════════════════════════════════════════════╝
```

### UDP 网络模式输出

```
╔══════════════════════════════════════════════╗
║   LOW-LATENCY MATCHING ENGINE - NETWORK MODE  ║
║    C++17 / Linux / UDP / Lock-Free / No-Mutex  ║
╚══════════════════════════════════════════════╝

[UDPServer] Listening on UDP 0.0.0.0:50000 (SO_RCVBUF=256KB)
[Status] pkts=1523 orders=152300 trades=148921 drops=0 errors=0
[Status] pkts=3046 orders=304600 trades=297842 drops=0 errors=0
...

╔══════════════════════════════════════════════╗
║            NETWORK STATISTICS                 ║
╠══════════════════════════════════════════════╣
║  Packets Received:          9876               ║
║  Bytes Received:      3357840 bytes            ║
║  Orders Dispatched:     987600                 ║
║  Queue Full Drops:          0                 ║
║  Parse Errors:              0                 ║
╚══════════════════════════════════════════════╝
```

### Benchmark 输出

```
========== HFT MATCHING ENGINE BENCHMARK ==========
Total Orders:        1000000
Throughput:          40066 ops/sec

Latency (MATCHING ONLY):
p50:                 133 ns
p90:                 104824 ns
p99:                 276400 ns
p99.9:               461243 ns

avg:                 24738 ns
min:                 56 ns
max:                 7132439 ns

Dropped orders:      0
==================================================

========== MATCHING ENGINE STATS ==========
Total Orders Processed: 1000000
Total Trades Executed:  563260
Slab Allocated:         301310
Slab Available:         2698690
============================================
```

## 设计决策与优化详解

### 1. 无锁 SPSC 环形队列 (`LockFreeQueue.h`)

- **目的**：消除 Producer ↔ Consumer 线程间的 mutex 竞争
- **原理**：原子 head/tail 下标 + `memory_order_acquire/release` 内存屏障
- **容量**：65,536 条目（2 的幂，可用位掩码替代取模运算）
- **优化**：head/tail 各占独立 Cache Line（`alignas(64)`），防止 False Sharing
- **自旋退避**：队列为空时调用 `_mm_pause()` 降低总线压力

### 2. 三级 Slab 分配器 (`SlabAllocator.h/.cpp`)

**替代传统 MemoryPool / malloc 的原因：**
- 传统单一大小内存池无法适配不同尺寸对象，Slab 按 Size Class 分级消除内部碎片
- 热路径上的 `new/delete` 会引入堆分配延迟和内存碎片

**架构：**

```
┌─────────────────────────────────────────────┐
│              SlabAllocator                   │
├──────────┬──────────┬───────────────────────┤
│  Slab[0]  │  Slab[1]  │     Slab[2]          │
│   32B     │   64B    │      128B             │
│ 100K块    │ 100K块   │    100K块 (Order用)   │
├──────────┼──────────┼───────────────────────┤
│ free_list │ free_list│    free_list          │
│ (CAS无锁) │ (CAS无锁)│     (CAS无锁)          │
│ allocated │ allocated│    allocated           │
│ available │ available│    available           │
└──────────┴──────────┴───────────────────────┘
```

- **3 个 Size Class**：32B / 64B / 128B，按对象大小自动选择（`select_class()`）
- **每 Slab 预分配 100,000 块**连续内存（`operator new[]` + `alignas(64)`）
- **Intrusive Free List**：空闲块首 8 字节存 next 指针，零额外开销
- **CAS 无锁分配/释放**：`compare_exchange_weak` 原子操作
- **Order 对象使用 128B Slab**（含 intrusive `next`/`prev` 指针后约 72 字节）

### 3. Price Ladder + Intrusive Linked List 订单簿 (`OrderBook.h/.cpp`)

**从 std::map+deque 升级的动机：**

| 维度 | 旧: std::map + deque | 新: PriceLadder + IntrusiveList |
|------|---------------------|----------------------------------|
| 内存分配 | 每次 insert = map节点 + deque节点（堆 malloc） | **零堆分配**（预分配固定数组） |
| Cache Locality | 差（指针分散在各处） | **优秀**（连续数组 + 内联指针） |
| FIFO 取单 | `deque.front()` + `pop_front()` | `list_pop_front()` 直接指针操作 |
| 撤单 | `deque.erase(linear_search)` | `list_unlink()` O(1) 直接断链 |

**数据结构设计：**

```cpp
// Price Ladder: 固定大小数组，O(1) 索引访问
static constexpr int64_t MIN_PRICE = 0;
static constexpr int64_t MAX_PRICE = 100000;
static constexpr size_t NUM_LEVELS = 100001;

PriceLevel bids_[NUM_LEVELS];   // 买盘价格阶梯
PriceLevel asks_[NUM_LEVELS];   // 卖盘价格阶梯

// 每个 Price Level 是一个 Intrusive Linked List
struct PriceLevel {
    Order* head;     // 链头（最早挂单 = 最优先成交）
    Order* tail;     // 链尾（最新挂单）
    size_t count;    // 该价位订单数
};
```

**Intrusive List 操作（全部 O(1)，内联在头文件中）：**

| 操作 | 复杂度 | 说明 |
|------|--------|------|
| `list_push_back(level, order)` | O(1) | 挂单追加到队尾（FIFO） |
| `list_pop_front(level)` | O(1) | 取最早 maker 成交 |
| `list_unlink(level, order)` | O(1) | 撤单从任意位置断链 |

**Order 结构体内嵌链表指针：**

```cpp
struct alignas(64) Order {
    // ... 订单字段 ...
    Order* next{nullptr};    // intrusive list 后继
    Order* prev{nullptr};    // intrusive list 前驱
};
```

### 4. UDP 网络层 (`UDPServer.h/.cpp`)

- **协议**：二进制线缆格式 `WireOrder`（`#pragma pack(push, 1)`，34 字节/订单）
- **Socket**：Non-blocking UDP + `SO_REUSEADDR` + 256KB `SO_RCVBUF`
- **批量解析**：单个 UDP 包可包含多个订单（按 `sizeof(WireOrder)` 切分）
- **解耦**：独立 `recv_thread_` 通过 SPSC LockFreeQueue 将订单推送给撮合线程
- **统计**：packets / bytes / dispatched / parse_errors / queue_full_drops 五维监控

### 5. Cache Line 对齐 (`alignas(64)`) 应用清单

| 结构体 | 对齐目标 |
|--------|---------|
| `Order` | 防止相邻订单间 False Sharing |
| `SlabAllocator::SlabHeader` | 隔离 free_list/allocated/available 到独立缓存行 |
| `EngineStats` | 分离各原子计数器到独立 Cache Line |
| `LockFreeQueue::head/tail` | Producer/Consumer 写分离 |
| `NetworkStats` | 分离 packets/bytes 与 drops/errors 计数器 |

### 6. 价格-时间优先撮合算法 (`MatchingEngine.cpp`)

```
限价买入:
  while (order存活 && 卖盘非空):
    最优卖价 = best_ask()        // 扫描 asks_[], 找到最低非空价
    if (最优卖价 > 限价):
      break                       // 无法成交
    level = ask_level(最优卖价)    // O(1): 数组索引
    maker = list_pop_front(*level) // O(1): 取最早挂单
    成交量 = min(剩余量, maker剩余)
    order->fill(成交量); maker->fill(成交量);
    if maker 已完全成交: slab_.deallocate(maker)
    if taker 已完全成交: break

市价买入: 同上但跳过价格检查（激进成交）
限价卖出: 对称逻辑，匹配买盘 best_bid()（最高买入价）
```

### 7. 热/温/冷路径分离

| 路径 | 操作特征 | 说明 |
|------|---------|------|
| **热路径**（撮合内循环） | 指针解引用、整数比较、fill 操作 | 无系统调用、无堆分配、无 I/O |
| **温路径**（内存池） | CAS 操作、placement new | 每次 alloc/dealloc 仅一次原子操作 |
| **冷路径**（日志/统计） | std::cout、atomic fetch_add | 异步线程执行，不阻塞撮合 |

### 8. 编译优化标志（Release）

```
-O3              : 最高优化级别
-march=native    : 启用 CPU 特有指令集（AVX2, BMI2 等）
-mtune=native    : 针对宿主 CPU 的调度优化
-funroll-loops   : 撮合内循环展开
-ftree-vectorize : 自动向量化
-pthread         : POSIX 线程支持
```

## 关键数据结构

### Order 订单 (`include/Order.h`)

```cpp
struct alignas(64) Order {
    uint64_t order_id;       // 订单 ID
    Side side;               // Buy | Sell
    OrderType type;          // Limit | Market | Cancel
    int64_t price;           // 价格
    uint32_t quantity;       // 委托数量
    uint32_t remaining_qty;  // 剩余数量
    uint32_t filled_qty;     // 已成交数量
    uint64_t timestamp_ns;   // 时间戳（纳秒）
    OrderStatus status;      // New | PartiallyFilled | FullyFilled | Cancelled
};
```

### WireOrder 线缆协议 (`include/UDPServer.h`)

```cpp
#pragma pack(push, 1)
struct WireOrder {
    uint64_t order_id;       // 8 字节
    uint8_t  side;           // 1 字节 (0=Buy, 1=Sell)
    uint8_t  type;           // 1 字节 (0=Limit, 1=Market, 2=Cancel)
    int64_t  price;          // 8 字节
    uint32_t quantity;       // 4 字节
    uint32_t padding;        // 4 字节（对齐填充）
    uint64_t timestamp_ns;   // 8 字节
};                          // 总计 34 字节
#pragma pack(pop)
```

### 并发模型 — 竞争点消除

```
Producer Thread                 Consumer Thread (唯一写入者)
┌─────────────────┐            ┌─────────────────────────────┐
│  OrderGenerator  │            │     MatchingEngine           │
│  / UDPServer     │──push()──▶│  process_order()             │
│                  │   无锁    │    ├─ match_limit_buy/sell   │
│                  │   队列     │    ├─ match_market_buy/sell  │
│                  │           │    ├─ handle_cancel          │
│                  │           │    └─ record_trade()         │
└─────────────────┘            └──────────┬──────────────────┘
                                          │
Logger Thread                            trades[]
┌─────────────────┐                      │
│  轮询 atomic    │◀─── async ──────────┘
│  输出摘要       │
└─────────────────┘
```

**已消除的竞争点：**
- Producer ↔ Queue：无锁 push（仅原子 tail 更新）
- Queue ↔ Consumer：无锁 pop（仅原子 head 更新）
- Stats 更新：`memory_order_relaxed` 原子操作（无屏障开销）
- Logger：异步读取原子变量，完全不阻塞

## 扩展方向

生产环境可进一步完善的方面：

1. **内核旁路网络**：用 DPDK / Solarflare Onload 替代标准 socket
2. **持久化**：WAL（Write-Ahead Log）实现崩溃恢复
3. **风控引擎**：持仓限制、信用检查（接单前拦截）
4. **熔断机制**：Kill Switch、熔断器
5. **多品种支持**：按交易标的分片多引擎
6. **持久化订单簿**：从日志快照恢复
7. **无锁数据结构升级**：用并发跳表或 Flat Combine Map 替代 std::map

## 许可证

本项目作为教育与演示用途的开源项目，供本人面试和作品集展示。

## 技术栈

C++17 / Linux / STL / CMake / 多线程编程 / Atomic / Perf / Socket / UDP
