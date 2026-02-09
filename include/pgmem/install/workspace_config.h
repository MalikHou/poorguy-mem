#pragma once

#include <string>

namespace pgmem::install {

struct InstallOptions {
    std::string workspace_root;
    std::string mcp_url{"http://127.0.0.1:8765/mcp"};
    std::string mcp_name{"poorguy-mem"};
    std::string pgmemd_bin{"pgmemd"};
    bool configure_codex{true};
    bool manage_systemd{true};
};

class WorkspaceConfigurator {
public:
    bool Install(const InstallOptions& options, std::string* error) const;
    bool Uninstall(const InstallOptions& options, std::string* error) const;

private:
    bool ConfigureCursorMcp(const InstallOptions& options, std::string* error) const;
    bool RemoveCursorMcp(const InstallOptions& options, std::string* error) const;

    bool ConfigureCursorRule(const InstallOptions& options, std::string* error) const;
    bool RemoveCursorRule(const InstallOptions& options, std::string* error) const;

    bool ConfigureCodexMcp(const InstallOptions& options, std::string* error) const;
    bool RemoveCodexMcp(const InstallOptions& options, std::string* error) const;

    bool WriteSystemdUnit(const InstallOptions& options, std::string* error) const;
    bool RemoveSystemdUnit(std::string* error) const;

    bool RunCommand(const std::string& cmd, std::string* output, int* exit_code) const;
};

}  // namespace pgmem::install
