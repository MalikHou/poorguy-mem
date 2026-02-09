#!/usr/bin/env bash
set -euo pipefail

curl -s http://127.0.0.1:8765/health | tee /tmp/pgmem_health.json

curl -s http://127.0.0.1:8765/mcp -X POST -H 'Content-Type: application/json' -d '{
  "id": 1,
  "method": "memory.commit_turn",
  "params": {
    "workspace_id": "demo",
    "session_id": "s1",
    "user_text": "please remember retry strategy",
    "assistant_text": "using exponential backoff",
    "code_snippets": ["int retry = 5;"],
    "commands": ["ctest --output-on-failure"]
  }
}' | tee /tmp/pgmem_commit.json

curl -s http://127.0.0.1:8765/mcp -X POST -H 'Content-Type: application/json' -d '{
  "id": 2,
  "method": "memory.search",
  "params": {
    "workspace_id": "demo",
    "query": "retry backoff",
    "top_k": 3,
    "token_budget": 1200
  }
}' | tee /tmp/pgmem_search.json

echo "Smoke test completed"
