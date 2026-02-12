#include "pgmem/install/workspace_config.h"

#include <sys/wait.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace pgmem::install {
namespace {

namespace fs = std::filesystem;

bool LooksLikeCommandNotFound(const std::string& output, const std::string& command) {
    const bool has_cmd       = output.find(command) != std::string::npos;
    const bool has_not_found = output.find("not found") != std::string::npos ||
                               output.find("command not found") != std::string::npos ||
                               output.find("No such file or directory") != std::string::npos;
    return has_cmd && has_not_found;
}

bool LooksLikeCodexUnavailable(const std::string& output) {
    if (LooksLikeCommandNotFound(output, "codex")) {
        return true;
    }
    const bool has_permission_issue = output.find("Permission denied") != std::string::npos ||
                                      output.find("failed to write MCP servers") != std::string::npos ||
                                      output.find("failed to persist config.toml") != std::string::npos;
    return has_permission_issue;
}

std::string RuleContent() {
    return R"(---
description: poorguy-mem memory server contract
alwaysApply: true
---
`poorguy-mem` is the workspace memory MCP server.

Execution contract:
1. If schema is unknown, call `memory.describe` first and follow its contract.
2. At task/session start, call `memory.bootstrap` with current `workspace_id` and `session_id`.
3. Before asking for repeated context, call `memory.search` to recall prior facts.
4. After each meaningful assistant turn, call `memory.commit_turn` with:
   `workspace_id`, `session_id`, `user_text`, `assistant_text`, plus useful `code_snippets` and `commands`.
5. If user marks information as important, call `memory.pin`.
6. For diagnostics only, call `memory.stats` or `memory.compact`.

Protocol constraints:
- Use only `POST /mcp` with `memory.*` methods.
- `/sync/push` and `/sync/pull` are removed and must be treated as unavailable.
)";
}

fs::path CursorMcpPath(const InstallOptions& options) {
    return fs::path(options.workspace_root) / ".cursor" / "mcp.json";
}

fs::path CursorRulePath(const InstallOptions& options) {
    return fs::path(options.workspace_root) / ".cursor" / "rules" / "poorguy-mem.mdc";
}

fs::path UserSystemdPath() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return {};
    }
    return fs::path(home) / ".config" / "systemd" / "user" / "pgmemd.service";
}

}  // namespace

bool WorkspaceConfigurator::Install(const InstallOptions& options, std::string* error) const {
    if (!ConfigureCursorMcp(options, error)) {
        return false;
    }
    if (!ConfigureCursorRule(options, error)) {
        return false;
    }
    if (options.manage_systemd) {
        if (!WriteSystemdUnit(options, error)) {
            return false;
        }
    }
    if (options.configure_codex && !ConfigureCodexMcp(options, error)) {
        return false;
    }
    return true;
}

bool WorkspaceConfigurator::Uninstall(const InstallOptions& options, std::string* error) const {
    if (!RemoveCursorMcp(options, error)) {
        return false;
    }
    if (!RemoveCursorRule(options, error)) {
        return false;
    }
    if (options.manage_systemd) {
        if (!RemoveSystemdUnit(error)) {
            return false;
        }
    }
    if (options.configure_codex && !RemoveCodexMcp(options, error)) {
        return false;
    }
    return true;
}

bool WorkspaceConfigurator::ConfigureCursorMcp(const InstallOptions& options, std::string* error) const {
    const fs::path path = CursorMcpPath(options);
    fs::create_directories(path.parent_path());

    boost::property_tree::ptree root;
    if (fs::exists(path)) {
        try {
            boost::property_tree::read_json(path.string(), root);
        } catch (const std::exception& ex) {
            if (error != nullptr) {
                *error = std::string("failed to parse ") + path.string() + ": " + ex.what();
            }
            return false;
        }
    }

    boost::property_tree::ptree servers;
    if (auto existing = root.get_child_optional("mcpServers")) {
        servers = *existing;
    }

    boost::property_tree::ptree server;
    server.put("url", options.mcp_url);
    server.put(
        "description",
        "Local memory server for this workspace. Use memory.describe/bootstrap/search/commit_turn/pin/stats/compact.");
    servers.put_child(options.mcp_name, server);

    root.put_child("mcpServers", servers);

    try {
        boost::property_tree::write_json(path.string(), root);
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = std::string("failed to write ") + path.string() + ": " + ex.what();
        }
        return false;
    }

    return true;
}

bool WorkspaceConfigurator::RemoveCursorMcp(const InstallOptions& options, std::string* error) const {
    const fs::path path = CursorMcpPath(options);
    if (!fs::exists(path)) {
        return true;
    }

    boost::property_tree::ptree root;
    try {
        boost::property_tree::read_json(path.string(), root);
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = std::string("failed to parse ") + path.string() + ": " + ex.what();
        }
        return false;
    }

    auto servers_opt = root.get_child_optional("mcpServers");
    if (!servers_opt) {
        return true;
    }

    servers_opt->erase(options.mcp_name);
    if (servers_opt->empty()) {
        root.erase("mcpServers");
    } else {
        root.put_child("mcpServers", *servers_opt);
    }

    try {
        boost::property_tree::write_json(path.string(), root);
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = std::string("failed to write ") + path.string() + ": " + ex.what();
        }
        return false;
    }

    return true;
}

bool WorkspaceConfigurator::ConfigureCursorRule(const InstallOptions& options, std::string* error) const {
    const fs::path path = CursorRulePath(options);
    fs::create_directories(path.parent_path());

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        if (error != nullptr) {
            *error = std::string("failed to open ") + path.string();
        }
        return false;
    }
    out << RuleContent();
    return true;
}

bool WorkspaceConfigurator::RemoveCursorRule(const InstallOptions& options, std::string* error) const {
    const fs::path path = CursorRulePath(options);
    if (!fs::exists(path)) {
        return true;
    }
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        if (error != nullptr) {
            *error = std::string("failed to remove ") + path.string() + ": " + ec.message();
        }
        return false;
    }
    return true;
}

bool WorkspaceConfigurator::ConfigureCodexMcp(const InstallOptions& options, std::string* error) const {
    std::string output;
    int code = 0;
    if (!RunCommand("codex mcp get " + options.mcp_name, &output, &code)) {
        if (error != nullptr) {
            *error = "failed to run codex mcp get";
        }
        return false;
    }

    if (code == 0) {
        return true;
    }

    if (LooksLikeCodexUnavailable(output)) {
        std::cerr << "[pgmem-install] warning: codex CLI unavailable, skipping Codex MCP registration\n";
        return true;
    }

    const std::string add_cmd = "codex mcp add " + options.mcp_name + " --url " + options.mcp_url;
    if (!RunCommand(add_cmd, &output, &code)) {
        if (error != nullptr) {
            *error = "failed to run codex mcp add";
        }
        return false;
    }

    if (code != 0) {
        if (LooksLikeCodexUnavailable(output)) {
            std::cerr << "[pgmem-install] warning: codex CLI unavailable, skipping Codex MCP registration\n";
            return true;
        }
        if (error != nullptr) {
            *error = "codex mcp add failed: " + output;
        }
        return false;
    }

    return true;
}

bool WorkspaceConfigurator::RemoveCodexMcp(const InstallOptions& options, std::string* error) const {
    std::string output;
    int code = 0;
    if (!RunCommand("codex mcp remove " + options.mcp_name, &output, &code)) {
        if (error != nullptr) {
            *error = "failed to run codex mcp remove";
        }
        return false;
    }

    if (LooksLikeCodexUnavailable(output)) {
        std::cerr << "[pgmem-install] warning: codex CLI unavailable, skipping Codex MCP removal\n";
        return true;
    }

    // If the server is already absent, codex may return non-zero; treat that as acceptable.
    return true;
}

bool WorkspaceConfigurator::WriteSystemdUnit(const InstallOptions& options, std::string* error) const {
    const fs::path unit_path = UserSystemdPath();
    if (unit_path.empty()) {
        if (error != nullptr) {
            *error = "HOME environment variable is not set";
        }
        return false;
    }

    fs::create_directories(unit_path.parent_path());

    std::ofstream out(unit_path, std::ios::trunc);
    if (!out.is_open()) {
        if (error != nullptr) {
            *error = std::string("failed to open ") + unit_path.string();
        }
        return false;
    }

    out << "[Unit]\n";
    out << "Description=Poorguy Memory Daemon\n";
    out << "After=network.target\n\n";
    out << "[Service]\n";
    out << "Type=simple\n";
    out << "ExecStart=" << options.pgmemd_bin << " --host " << config::kDefaultHost << " --port "
        << config::kDefaultMcpPort << " --store-backend " << config::kDefaultStoreBackend << " --core-number "
        << options.core_number << " --enable-io-uring-network-engine "
        << (options.enable_io_uring_network_engine ? "true" : "false") << " --mem-budget-mb "
        << config::kDefaultMemBudgetMb << " --disk-budget-gb " << config::kDefaultDiskBudgetGb
        << " --gc-high-watermark " << config::kDefaultGcHighWatermark << " --gc-low-watermark "
        << config::kDefaultGcLowWatermark << " --gc-batch-size " << config::kDefaultGcBatchSize
        << " --max-record-bytes " << config::kDefaultMaxRecordBytes << " --enable-tombstone-gc "
        << (config::kDefaultEnableTombstoneGc ? "true" : "false") << " --shutdown-drain-timeout-ms "
        << config::kDefaultShutdownDrainTimeoutMs << " --store-root " << options.workspace_root << "/.pgmem/store";
    if (!options.pgmemd_extra_args.empty()) {
        out << " " << options.pgmemd_extra_args;
    }
    out << "\n";
    out << "Restart=on-failure\n";
    out << "RestartSec=2\n\n";
    out << "[Install]\n";
    out << "WantedBy=default.target\n";

    std::string output;
    int code = 0;
    if (!RunCommand("systemctl --user daemon-reload", &output, &code)) {
        if (error != nullptr) {
            *error = "failed to run systemctl --user daemon-reload";
        }
        return false;
    }

    RunCommand("systemctl --user enable --now pgmemd.service", &output, &code);
    (void)code;

    return true;
}

bool WorkspaceConfigurator::RemoveSystemdUnit(std::string* error) const {
    std::string output;
    int code = 0;
    RunCommand("systemctl --user disable --now pgmemd.service", &output, &code);

    const fs::path unit_path = UserSystemdPath();
    if (!unit_path.empty()) {
        std::error_code ec;
        fs::remove(unit_path, ec);
        if (ec && error != nullptr) {
            *error = "failed to remove systemd unit: " + ec.message();
            return false;
        }
    }

    RunCommand("systemctl --user daemon-reload", &output, &code);
    return true;
}

bool WorkspaceConfigurator::RunCommand(const std::string& cmd, std::string* output, int* exit_code) const {
    std::string data;
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        data += buffer;
    }

    const int status = pclose(pipe);
    if (output != nullptr) {
        *output = data;
    }
    if (exit_code != nullptr) {
        if (status == -1) {
            *exit_code = 1;
        } else if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
        } else {
            *exit_code = 1;
        }
    }
    return true;
}

}  // namespace pgmem::install
