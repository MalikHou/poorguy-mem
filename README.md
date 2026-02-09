# poorguy-mem

Poorguy-Mem v1 is a C++17 Agent Memory system for Cursor + Codex.

It provides:
- Local MCP server (`pgmemd`) at `http://127.0.0.1:8765/mcp`
- Remote sync service (`pgmem-syncd`) for async incremental sync
- Auto installer (`pgmem-install`) for Cursor workspace config + Codex MCP registration
- Hybrid retrieval (`hash semantic` + `BM25 lexical` fallback path)
- Storage adapter abstraction with compile-time pluggable backend

## Implemented architecture

- `pgmemd`
  - MCP tools: `memory.bootstrap`, `memory.commit_turn`, `memory.search`, `memory.pin`, `memory.stats`
  - local-first writes, async sync queue, metrics snapshot
- `pgmem-syncd`
  - `/sync/push`, `/sync/pull` endpoints
  - LWW conflict policy (`updated_at_ms`, `version`, `node_id`) + `audit_log` persistence
- `pgmem-install`
  - merges workspace `.cursor/mcp.json` idempotently
  - writes `.cursor/rules/poorguy-mem.mdc`
  - registers Codex MCP via `codex mcp add` when enabled
  - installs/removes `~/.config/systemd/user/pgmemd.service`

## Repository layout

- `include/pgmem/...`: interfaces and core headers
- `src/apps/`: `pgmemd`, `pgmem-syncd`, `pgmem-install`
- `src/core/`: memory engine, retriever, summary, sync, metrics
- `src/store/`: in-memory adapter + EloqStore adapter
- `src/net/`: lightweight HTTP server/client
- `src/mcp/`: MCP dispatcher
- `src/install/`: Cursor/Codex/systemd auto config
- `tests/`: unit tests
- `third_party/eloqstore`: pinned submodule

## EloqStore submodule

Pinned at:
- `third_party/eloqstore` commit `7a293bbb216fe6629cbf576b207c373224c2b344`

Update submodule after clone:

```bash
git submodule update --init --recursive
```

## Build

### Backend selection (compile-time)

`PGMEM_STORE_BACKEND` supports:
- `eloqstore` (default)
- `inmemory`
- `custom`

### Recommended build script

Use `scripts/build.sh` to avoid repeating options and always generate `compile_commands.json`:

```bash
# default: eloqstore + Release + tests ON
scripts/build.sh

# local validation
scripts/build.sh --backend inmemory --build-type Debug

# custom backend
scripts/build.sh \
  --backend custom \
  --custom-sources "/abs/path/custom_store_adapter.cpp" \
  --build-dir build-custom
```

Script behavior:
- Runs CMake with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`
- Builds with `cmake --build`
- Runs tests by default (`ctest`)
- Links `<repo>/compile_commands.json` to `<build-dir>/compile_commands.json`

### 1) In-memory backend (fast local validation)

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DPGMEM_STORE_BACKEND=inmemory
cmake --build build -j
ctest --test-dir build --output-on-failure
ln -snf "$(pwd)/build/compile_commands.json" "$(pwd)/compile_commands.json"
```

### 2) EloqStore backend (default production path)

You need EloqStore runtime/build dependencies (`glog`/`jsoncpp`/`liburing`/`aws-sdk-cpp`/`zstd`/etc.).

```bash
cmake -S . -B build-eloq \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DPGMEM_STORE_BACKEND=eloqstore
cmake --build build-eloq -j
ln -snf "$(pwd)/build-eloq/compile_commands.json" "$(pwd)/compile_commands.json"
```

If dependencies are missing, CMake fails at discovery (for example missing `AWSSDKConfig.cmake`).

### 3) Custom backend

Provide your backend source files via CMake:

```bash
cmake -S . -B build-custom \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DPGMEM_STORE_BACKEND=custom \
  -DPGMEM_CUSTOM_STORE_SOURCES=/abs/path/to/custom_store_adapter.cpp
cmake --build build-custom -j
ln -snf "$(pwd)/build-custom/compile_commands.json" "$(pwd)/compile_commands.json"
```

Your custom source must implement:
- `pgmem::store::CreateCustomStoreAdapter(const StoreAdapterConfig&, std::string*)`

Reference template:
- `assets/custom_store_adapter_example.cpp`

## Run

### Start remote sync service

```bash
./build/pgmem-syncd \
  --host 127.0.0.1 \
  --port 9765 \
  --store-backend eloqstore \
  --store-root ./.pgmem/sync-store \
  --store-threads 0 \
  --node-id syncd
```

### Start local MCP server

```bash
./build/pgmemd \
  --host 127.0.0.1 \
  --port 8765 \
  --store-backend eloqstore \
  --store-root ./.pgmem/store \
  --store-threads 0 \
  --node-id local \
  --sync-host 127.0.0.1 \
  --sync-port 9765
```

## Local storage directories

Default local paths are relative to current working directory (or workspace for installer-managed service):

- `pgmemd`: `.pgmem/store`
- `pgmem-syncd`: `.pgmem/sync-store`
- Installer-managed service (`pgmem-install` + systemd): `<workspace>/.pgmem/store`

Override with:
- `--store-root /absolute/or/relative/path`

When S3 is enabled, local disk path is still used for local cache/state; S3 is a remote durable tier.

### EloqStore local mode vs S3 mode

- No S3 config: local `mem + disk` (no cloud tier)
- With S3 config: local cache + S3 durable tier

Example S3 mode:

```bash
./build/pgmemd \
  --host 127.0.0.1 \
  --port 8765 \
  --store-backend eloqstore \
  --store-root ./.pgmem/store \
  --store-threads 0 \
  --s3-bucket-path mybucket/pgmem \
  --s3-provider aws \
  --s3-endpoint http://127.0.0.1:9000 \
  --s3-region us-east-1 \
  --s3-access-key minioadmin \
  --s3-secret-key minioadmin \
  --s3-verify-ssl false \
  --node-id local
```

You can also provide credentials via env:
- `PGMEM_S3_ACCESS_KEY`
- `PGMEM_S3_SECRET_KEY`

### Per-thread-per-core recommendation

- `--store-threads 0` means auto: `num_threads = hardware_concurrency()`
- This maps well to EloqStore's shard/thread architecture and is the recommended default
- Set explicit thread count only when co-locating with other heavy services on the same host

Health check:

```bash
curl -s http://127.0.0.1:8765/health
```

## Install into Cursor + Codex

```bash
./build/pgmem-install --workspace "$(pwd)" --pgmemd-bin "$(pwd)/build/pgmemd"
```

For container/non-systemd environments:

```bash
./build/pgmem-install --workspace "$(pwd)" --pgmemd-bin "$(pwd)/build/pgmemd" --no-systemd
```

Uninstall:

```bash
./build/pgmem-install --workspace "$(pwd)" --uninstall
```

Installer behavior:
- Writes or merges `.cursor/mcp.json`
- Writes `.cursor/rules/poorguy-mem.mdc`
- Runs `codex mcp add poorguy-mem --url http://127.0.0.1:8765/mcp` if missing
- Manages `systemd --user` service by default

### Configuration tool usage (`pgmem-install`)

Check all options:

```bash
./build/pgmem-install --help
```

Common options:
- `--workspace <path>`: target workspace root
- `--url <mcp-url>`: URL written into `.cursor/mcp.json`
- `--pgmemd-bin <path>`: binary path used in generated systemd unit
- `--no-codex`: skip Codex registration
- `--no-systemd`: skip systemd unit management and post-install health check
- `--uninstall`: remove Cursor/Codex/systemd setup

## Usage practices

Recommended baseline for stable latency:
- Keep `--store-threads 0` (auto per-thread-per-core)
- Keep local store on fast SSD
- Use S3 only when cross-host durability/sync is needed
- Keep `pgmemd` and `pgmem-syncd` on separate ports (`8765` / `9765`)

Recommended workflow:
1. Build using `scripts/build.sh --backend eloqstore`.
2. Start `pgmem-syncd`, then start `pgmemd`.
3. Run `pgmem-install` once per workspace.
4. Validate with `scripts/smoke_mcp.sh`.

## MCP request examples

### `memory.commit_turn`

```bash
curl -s http://127.0.0.1:8765/mcp -X POST -H 'Content-Type: application/json' -d '{
  "id": 1,
  "method": "memory.commit_turn",
  "params": {
    "workspace_id": "demo",
    "session_id": "s1",
    "user_text": "add retry logic",
    "assistant_text": "implemented exponential backoff",
    "code_snippets": ["int retry = 3;"],
    "commands": ["ctest --output-on-failure"]
  }
}'
```

### `memory.search`

```bash
curl -s http://127.0.0.1:8765/mcp -X POST -H 'Content-Type: application/json' -d '{
  "id": 2,
  "method": "memory.search",
  "params": {
    "workspace_id": "demo",
    "query": "retry backoff",
    "top_k": 5,
    "token_budget": 1500
  }
}'
```

## Data model namespaces

- `mem_items`
- `mem_embeddings`
- `mem_lexicon`
- `session_summaries`
- `sync_outbox`
- `audit_log`

## Notes

- Code is C++17.
- Storage backend is compile-time pluggable; default is `eloqstore`.
- Read path lock contention was reduced by using immutable snapshot retrieval in `HybridRetriever` (copy-on-write update, lock-free scoring path).
- `EloqStoreAdapter` hot operations no longer serialize on a global mutex; only startup/shutdown is state-gated.
- Secret-like patterns are redacted before persistence.
