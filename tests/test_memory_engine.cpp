#include "test_framework.h"

#include <memory>

#include "pgmem/core/memory_engine.h"
#include "pgmem/core/retriever.h"
#include "pgmem/store/store_adapter.h"

TEST_CASE(test_memory_engine_commit_and_search) {
    auto store = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), nullptr, "node-a");

    pgmem::CommitTurnInput input;
    input.workspace_id = "ws";
    input.session_id = "sess";
    input.user_text = "Please add retry logic";
    input.assistant_text = "I will add exponential backoff";
    input.code_snippets = {"int retry = 3;"};
    input.commands = {"make test"};

    const auto commit_out = engine.CommitTurn(input);
    ASSERT_EQ(commit_out.stored_ids.size(), 1U);

    pgmem::SearchInput search;
    search.workspace_id = "ws";
    search.query = "retry backoff";
    search.top_k = 3;
    search.token_budget = 2048;

    const auto search_out = engine.Search(search);
    ASSERT_TRUE(!search_out.hits.empty());
    ASSERT_EQ(search_out.hits[0].memory_id, commit_out.stored_ids[0]);
}

TEST_CASE(test_memory_engine_pin) {
    auto store = pgmem::store::CreateInMemoryStoreAdapter();
    auto retriever = pgmem::core::CreateHybridRetriever();

    pgmem::core::MemoryEngine engine(std::move(store), std::move(retriever), nullptr, "node-a");

    pgmem::CommitTurnInput input;
    input.workspace_id = "ws";
    input.session_id = "sess";
    input.user_text = "remember this";
    input.assistant_text = "done";

    const auto commit_out = engine.CommitTurn(input);
    ASSERT_EQ(commit_out.stored_ids.size(), 1U);

    std::string error;
    ASSERT_TRUE(engine.Pin("ws", commit_out.stored_ids[0], true, &error));
}
