#include "pgmem/install/workspace_config.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/wait.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace pgmem::install {
namespace {

namespace fs = std::filesystem;

std::string RuleContent() {
    return R"(---
description: poorguy-mem auto memory hooks
alwaysApply: true
---
When starting a new task, call `memory.bootstrap` first using the active workspace/session.
After each assistant turn, call `memory.commit_turn` with user/assistant text plus code snippets and commands.
Prefer `memory.search` before requesting additional user context.
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

    const std::string add_cmd = "codex mcp add " + options.mcp_name + " --url " + options.mcp_url;
    if (!RunCommand(add_cmd, &output, &code)) {
        if (error != nullptr) {
            *error = "failed to run codex mcp add";
        }
        return false;
    }

    if (code != 0) {
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
    out << "ExecStart=" << options.pgmemd_bin
        << " --host 127.0.0.1 --port 8765 --store-backend eloqstore --store-threads 0 --store-root "
        << options.workspace_root << "/.pgmem/store --node-id local\n";
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
