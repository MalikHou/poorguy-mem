#pragma once

#include <string>

#include "pgmem/config/defaults.h"

namespace pgmem::install {

struct InstallOptions {
    std::string pgmemd_bin{"pgmemd"};
    std::string pgmemd_extra_args;
    std::string store_root{config::kDefaultStoreRoot};
    int core_number{config::kDefaultCoreNumber};
    bool manage_systemd{true};
};

class WorkspaceConfigurator {
public:
    bool Install(const InstallOptions& options, std::string* error) const;
    bool Uninstall(const InstallOptions& options, std::string* error) const;

private:
    bool WriteSystemdUnit(const InstallOptions& options, std::string* error) const;
    bool RemoveSystemdUnit(std::string* error) const;

    bool RunCommand(const std::string& cmd, std::string* output, int* exit_code) const;
};

}  // namespace pgmem::install
