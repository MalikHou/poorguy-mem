# Memory MCP 测试计划（ZH）

## 1. 目标

本测试计划用于验证 Memory MCP 在功能正确性、接口稳定性、恢复能力和性能边界上的工程质量。

## 2. 范围

覆盖对象：
- MCP 接口：`memory.describe/write/query/pin/stats/compact`
- 检索链路：sparse + dense + rerank
- 存储后端：`eloqstore` 与 `inmemory`
- 容量治理：pin、TTL、tombstone、compact

## 3. 测试环境与前置条件

1. 依赖探测通过：
```bash
scripts/install_deps.sh --verify-only
```
2. 构建完成（建议开启测试）：
```bash
cmake -S . -B build -DBUILD_TESTING=ON -DPGMEM_STORE_BACKEND=eloqstore
cmake --build build -j"$(nproc)"
```
3. verify 流程会在每个 stage 前执行 runtime reset（`.pgmem/.mock-bin/.mock-state`）。

## 4. 单元测试矩阵

### U-01 Analyzer
- 分词规则正确性
- 大小写归一化
- 空输入处理

### U-02 Sparse 评分
- 词项匹配单调性
- 查询相关性排序正确性

### U-03 Dense 与重排
- 稠密候选可召回
- 多信号融合分数稳定

### U-04 Pin 治理
- pin/unpin 主路径
- 配额与比例约束
- pin 对排序与回收选择的影响

### U-05 TTL 与 Tombstone
- 过期记录过滤
- tombstone 记录不可检索

## 5. 集成测试场景

### I-01 最小闭环
`write -> query -> pin -> compact -> stats`

### I-02 工作区隔离
- 不同 `workspace_id` 间不可互查。

### I-03 后端一致性
- 同输入下，`eloqstore` 与 `inmemory` 返回结构一致。

### I-04 并发一致性
- 并发写读后，已写 token 可被最终检索到。

### I-05 启停与健康性
- `/health` 就绪语义正确。
- SIGTERM 后可在超时内退出。

## 6. 故障注入场景

1. 批写失败：验证写入失败可见性与返回语义。
2. 检查点异常：验证 replay 续跑能力。
3. 紧凑过程异常：验证 compact 中断后的继续可用性。
4. io_uring 不可用（eloqstore）：验证启动失败契约。

## 7. 性能验收目标

- 召回目标：`Recall@10 >= 0.95`
- 延迟目标：`p95 query <= 180ms`
- 写入目标：`p95 write <= 120ms`

说明：大规模压测建议在独立基准环境执行。

## 8. 执行入口

```bash
ctest --test-dir build --output-on-failure
scripts/start.sh verify
scripts/start.sh verify --keep-artifacts
```

## 9. 验收标准

1. 单元测试全部通过。
2. `scripts/start.sh verify` 全 stage 通过。
3. 覆盖率 gate 满足阈值（当前为 85%）。
4. 关键接口输出字段类型与 API 契约一致。
