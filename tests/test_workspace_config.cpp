#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include "pgmem/util/json.h"
#include "pgmem/install/workspace_config.h"

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
    options.workspace_root = tmp.string();
    options.configure_codex = false;
    options.manage_systemd = false;

    pgmem::install::WorkspaceConfigurator configurator;
    std::string error;

    ASSERT_TRUE(configurator.Install(options, &error));
    ASSERT_TRUE(configurator.Install(options, &error));

    const fs::path mcp_path = tmp / ".cursor" / "mcp.json";
    ASSERT_TRUE(fs::exists(mcp_path));

    {
        std::ifstream in(mcp_path);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        pgmem::util::Json json;
        std::string parse_error;
        ASSERT_TRUE(pgmem::util::ParseJson(text, &json, &parse_error));
        ASSERT_TRUE(json.get_child_optional("mcpServers.existing").has_value());
        ASSERT_TRUE(json.get_child_optional("mcpServers.poorguy-mem").has_value());
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

    fs::remove_all(tmp);
}
