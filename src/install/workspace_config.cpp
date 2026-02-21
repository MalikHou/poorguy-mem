#include "pgmem/install/workspace_config.h"

#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace pgmem::install {
namespace {

namespace fs = std::filesystem;

fs::path UserSystemdPath() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return {};
    }
    return fs::path(home) / ".config" / "systemd" / "user" / "pgmemd.service";
}

std::string ResolveStoreRoot(const std::string& store_root) {
    fs::path path = store_root.empty() ? fs::path(config::kDefaultStoreRoot) : fs::path(store_root);
    if (path.is_absolute()) {
        return path.string();
    }

    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        return (fs::path(home) / path).string();
    }
    return path.string();
}

}  // namespace

bool WorkspaceConfigurator::Install(const InstallOptions& options, std::string* error) const {
    if (!options.manage_systemd) {
        return true;
    }
    return WriteSystemdUnit(options, error);
}

bool WorkspaceConfigurator::Uninstall(const InstallOptions& options, std::string* error) const {
    if (!options.manage_systemd) {
        return true;
    }
    return RemoveSystemdUnit(error);
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

    const std::string store_root = ResolveStoreRoot(options.store_root);

    out << "[Unit]\n";
    out << "Description=Poorguy Memory Daemon\n";
    out << "After=network.target\n\n";
    out << "[Service]\n";
    out << "Type=simple\n";
    out << "ExecStart=" << options.pgmemd_bin << " --host " << config::kDefaultHost << " --port "
        << config::kDefaultMcpPort << " --store-backend " << config::kDefaultStoreBackend << " --core-number "
        << options.core_number << " --mem-budget-mb " << config::kDefaultMemBudgetMb << " --disk-budget-gb "
        << config::kDefaultDiskBudgetGb << " --gc-high-watermark " << config::kDefaultGcHighWatermark
        << " --gc-low-watermark " << config::kDefaultGcLowWatermark << " --gc-batch-size "
        << config::kDefaultGcBatchSize << " --max-record-bytes " << config::kDefaultMaxRecordBytes
        << " --enable-tombstone-gc " << (config::kDefaultEnableTombstoneGc ? "true" : "false") << " --store-root "
        << store_root;
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
