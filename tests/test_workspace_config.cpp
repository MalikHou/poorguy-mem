#include <filesystem>
#include <fstream>
#include <iterator>

#include "pgmem/install/workspace_config.h"
#include "pgmem/util/json.h"
#include "test_framework.h"

TEST_CASE(test_workspace_install_uninstall_idempotent) {
    namespace fs = std::filesystem;

    const fs::path tmp = fs::temp_directory_path() / "pgmem-config-test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / ".cursor");

    {
        std::ofstream out(tmp / ".cursor" / "mcp.json");
        out << "{\"mcpServers\":{\"existing\":{\"url\":\"http://127.0.0.1:1234/mcp\"}}}";
    }

    pgmem::install::InstallOptions options;
    options.workspace_root  = tmp.string();
    options.configure_codex = false;
    options.manage_systemd  = false;

    pgmem::install::WorkspaceConfigurator configurator;
    std::string error;

    ASSERT_TRUE(configurator.Install(options, &error));
    ASSERT_TRUE(configurator.Install(options, &error));

    const fs::path mcp_path  = tmp / ".cursor" / "mcp.json";
    const fs::path rule_path = tmp / ".cursor" / "rules" / "poorguy-mem.mdc";
    ASSERT_TRUE(fs::exists(mcp_path));
    ASSERT_TRUE(fs::exists(rule_path));

    {
        std::ifstream in(mcp_path);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        pgmem::util::Json json;
        std::string parse_error;
        ASSERT_TRUE(pgmem::util::ParseJson(text, &json, &parse_error));
        ASSERT_TRUE(json.get_child_optional("mcpServers.existing").has_value());
        ASSERT_TRUE(json.get_child_optional("mcpServers.poorguy-mem").has_value());
        ASSERT_TRUE(json.get_child_optional("mcpServers.poorguy-mem.description").has_value());
    }

    {
        std::ifstream in(rule_path);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ASSERT_TRUE(text.find("memory.describe") != std::string::npos);
        ASSERT_TRUE(text.find("workspace memory MCP server") != std::string::npos);
    }

    ASSERT_TRUE(configurator.Uninstall(options, &error));
    ASSERT_TRUE(configurator.Uninstall(options, &error));

    {
        std::ifstream in(mcp_path);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        pgmem::util::Json json;
        std::string parse_error;
        ASSERT_TRUE(pgmem::util::ParseJson(text, &json, &parse_error));
        ASSERT_TRUE(!json.get_child_optional("mcpServers.poorguy-mem"));
    }
    ASSERT_TRUE(!fs::exists(rule_path));

    fs::remove_all(tmp);
}
