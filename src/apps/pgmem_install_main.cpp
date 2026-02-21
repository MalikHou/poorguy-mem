#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "pgmem/config/defaults.h"
#include "pgmem/install/workspace_config.h"
#include "pgmem/net/http_client.h"

namespace {

struct Args {
    bool uninstall{false};
    bool manage_systemd{true};
    bool show_help{false};
    std::string pgmemd_bin{"pgmemd"};
    std::string pgmemd_extra_args;
    std::string store_root{pgmem::config::kDefaultStoreRoot};
    int core_number{pgmem::config::kDefaultCoreNumber};
    bool invalid{false};
    std::string invalid_message;
};

void PrintUsage() {
    std::cout << "Usage:\n"
              << "  pgmem-install [options]\n\n"
              << "Options:\n"
              << "  --pgmemd-bin <path>  pgmemd binary path for systemd unit (default: pgmemd)\n"
              << "  --pgmemd-extra-args <args>\n"
              << "                       Extra args appended to pgmemd systemd ExecStart\n"
              << "  --store-root <path>  Store root passed to pgmemd (default: " << pgmem::config::kDefaultStoreRoot
              << ")\n"
              << "  --core-number <N>    Worker/core mapping for daemon runtime (default: "
              << pgmem::config::kDefaultCoreNumber << ")\n"
              << "  --no-systemd         Skip systemd --user unit install/remove and health check\n"
              << "  --uninstall          Remove systemd user unit configuration\n"
              << "  --help, -h           Show this help message\n";
}

void MarkRemovedFlag(Args* args, const std::string& key, const std::string& guidance) {
    args->invalid         = true;
    args->invalid_message = "flag " + key + " is removed; " + guidance;
}

Args ParseArgs(int argc, char** argv) {
    Args args;

    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help" || key == "-h") {
            args.show_help = true;
        } else if (key == "--uninstall") {
            args.uninstall = true;
        } else if (key == "--no-systemd") {
            args.manage_systemd = false;
        } else if (key == "--pgmemd-bin" && i + 1 < argc) {
            args.pgmemd_bin = argv[++i];
        } else if (key == "--pgmemd-extra-args" && i + 1 < argc) {
            args.pgmemd_extra_args = argv[++i];
        } else if (key == "--store-root" && i + 1 < argc) {
            args.store_root = argv[++i];
        } else if (key == "--core-number" && i + 1 < argc) {
            args.core_number = std::atoi(argv[++i]);
        } else if (key == "--enable-io-uring") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "network io_uring is hard-wired to true and not configurable");
        } else if (key == "--store-partitions" || key == "--allow-inmemory-fallback" ||
                   key == "--event-dispatcher-num" || key == "--write-ack-mode" ||
                   key == "--volatile-flush-interval-ms" || key == "--volatile-max-pending-ops" ||
                   key == "--shutdown-drain-timeout-ms" || key == "--store-threads") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "local mode keeps these options internal-only");
        } else {
            args.invalid         = true;
            args.invalid_message = "unknown argument: " + key;
        }
    }

    return args;
}

bool HealthCheck() {
    pgmem::net::HttpClient http;
    for (int i = 0; i < 5; ++i) {
        const auto resp = http.Get("127.0.0.1", pgmem::config::kDefaultMcpPort, "/health", 1000);
        if (resp.ok) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = ParseArgs(argc, argv);
    if (args.invalid) {
        std::cerr << "pgmem install failed: " << args.invalid_message << "\n";
        return 2;
    }
    if (args.show_help) {
        PrintUsage();
        return 0;
    }

    pgmem::install::InstallOptions options;
    options.pgmemd_bin        = args.pgmemd_bin;
    options.pgmemd_extra_args = args.pgmemd_extra_args;
    options.store_root        = args.store_root;
    options.core_number       = args.core_number;
    options.manage_systemd    = args.manage_systemd;

    pgmem::install::WorkspaceConfigurator configurator;
    std::string error;

    if (args.uninstall) {
        if (!configurator.Uninstall(options, &error)) {
            std::cerr << "pgmem uninstall failed: " << error << "\n";
            return 1;
        }
        std::cout << "pgmem uninstall completed\n";
        return 0;
    }

    if (!configurator.Install(options, &error)) {
        std::cerr << "pgmem install failed: " << error << "\n";
        return 1;
    }

    if (options.manage_systemd) {
        if (!HealthCheck()) {
            std::cerr << "pgmem install warning: service health check failed on http://127.0.0.1:"
                      << pgmem::config::kDefaultMcpPort << "/health\n";
            return 2;
        }
    }

    std::cout << "pgmem install completed\n";
    return 0;
}
