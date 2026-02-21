# Memory MCP 查询链路设计（ZH）

## 1. 目标与生命周期（What / Why）

`memory.query` 的设计目标是：
1. 关键词可控召回（sparse）和语义召回（dense）同时成立。
2. 多信号统一打分，保证排序可解释。
3. 在给定 `token_budget` 下输出稳定、可消费的结果。

标准生命周期：
`输入解析 -> sparse 召回 -> dense 召回 -> merge -> rerank -> 过滤 -> budget 截断 -> 输出`

## 2. 执行模型（How）

执行主体：`HybridRetriever`（`src/core/retriever.cpp`）

输入主体：`QueryInput`（`include/pgmem/types.h`）
- `workspace_id`
- `query`
- `top_k`
- `token_budget`
- `filters`
- `recall`
- `rerank`
- `debug`

输出主体：`QueryOutput`
- `hits`
- `debug_stats`

## 3. Sparse 召回（分词 / 词项统计 / BM25-like）

流程：
1. `IAnalyzer` 对 `query` 分词。
2. 在 workspace 作用域内读取词项统计与文档词频。
3. 计算 lexical 分数（BM25-like）。
4. 选出 sparse 候选池（受 `recall.sparse_k` 与 `oversample` 影响）。

作用：
- 保障关键词与短语命中能力。
- 为 dense 召回不足场景提供稳定下界。

## 4. Dense 召回（embedding / LSH index / 候选生成）

流程：
1. `IEmbeddingProvider` 生成查询向量（默认 `HashEmbeddingProvider`）。
2. `IVectorIndex` 执行近邻搜索（默认 `LshVectorIndex`）。
3. 在 workspace 维度过滤候选。
4. 生成 dense 候选池（受 `recall.dense_k` 与 `oversample` 影响）。

作用：
- 补齐同义表达与语义相似命中。
- 与 sparse 形成互补。

## 5. 融合打分（`sparse/dense/freshness/pin/final`）

候选按 `memory_id` 合并后，计算分数组件：
- `sparse`：词项相关性
- `dense`：向量相似度
- `freshness`：时间衰减分
- `pin`：保留优先级分

最终分：

```text
final = w_sparse*sparse + w_dense*dense + w_freshness*freshness + w_pin*pin
```

权重来源：`QueryInput.rerank`，未传则使用默认值。

## 6. 过滤器与 `token_budget` 截断

过滤器字段：
- `session_id`
- `sources`
- `tags_any`
- `updated_after_ms`
- `updated_before_ms`
- `pinned_only`

执行顺序：
1. 候选排序后执行过滤。
2. 命中过期记录（TTL）直接剔除。
3. 逐条累积 token 估算，超过 `token_budget` 立即停止。

语义约束：
- `token_budget = 0` 表示不启用预算截断。

## 7. 调参指南（recall / rerank）

召回参数：
- `recall.sparse_k`：提高后增强关键词覆盖
- `recall.dense_k`：提高后增强语义覆盖
- `recall.oversample`：提高后增加 merge 前候选冗余

重排参数：
- `w_sparse`：提高后增强字面相关性
- `w_dense`：提高后增强语义相关性
- `w_freshness`：提高后增强时效偏好
- `w_pin`：提高后增强 pin 优先级

调参建议：
1. 术语类问答优先提高 `w_sparse`。
2. 语义扩展类任务优先提高 `w_dense`。
3. 操作手册与规章类场景可提高 `w_pin`。

## 8. 可观测性（`debug_stats` 字段与定位方法）

`debug_stats` 字段：
- `sparse_candidates`
- `dense_candidates`
- `merged_candidates`
- `latency_ms.sparse`
- `latency_ms.dense`
- `latency_ms.rerank`
- `latency_ms.total`

定位方法：
1. `sparse_candidates` 低且命中差：先检查 analyzer 与词项覆盖。
2. `dense_candidates` 低：检查 embedding 维度与向量索引参数。
3. `latency_ms.total` 高：先看 `dense` 与 `rerank` 子耗时占比。
4. `merged_candidates` 远小于召回总和：检查过滤器是否过严。
