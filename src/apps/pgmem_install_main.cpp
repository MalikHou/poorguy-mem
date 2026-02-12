#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "pgmem/config/defaults.h"
#include "pgmem/install/workspace_config.h"
#include "pgmem/net/http_client.h"

namespace {

std::string ToLower(std::string v) {
    for (char& c : v) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return v;
}

bool ParseBoolFlag(const std::string& value, bool fallback) {
    const std::string v = ToLower(value);
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        return false;
    }
    return fallback;
}

struct Args {
    bool uninstall{false};
    bool configure_codex{true};
    bool manage_systemd{true};
    bool show_help{false};
    std::string workspace;
    std::string url{pgmem::config::kDefaultMcpUrl};
    std::string pgmemd_bin{"pgmemd"};
    std::string pgmemd_extra_args;
    int core_number{pgmem::config::kDefaultCoreNumber};
    bool enable_io_uring_network_engine{pgmem::config::kDefaultEnableIoUringNetworkEngine};
    bool invalid{false};
    std::string invalid_message;
};

void PrintUsage() {
    std::cout << "Usage:\n"
              << "  pgmem-install [options]\n\n"
              << "Options:\n"
              << "  --workspace <path>   Workspace root to configure (default: current directory)\n"
              << "  --url <mcp-url>      MCP URL written to .cursor/mcp.json (default: "
              << pgmem::config::kDefaultMcpUrl << ")\n"
              << "  --pgmemd-bin <path>  pgmemd binary path for systemd unit (default: pgmemd)\n"
              << "  --pgmemd-extra-args <args>\n"
              << "                       Extra args appended to pgmemd systemd ExecStart\n"
              << "  --core-number <N>    Worker/core mapping for daemon runtime (default: "
              << pgmem::config::kDefaultCoreNumber << ")\n"
              << "  --enable-io-uring-network-engine <bool>\n"
              << "                       Enable brpc network io_uring engine (default: "
              << (pgmem::config::kDefaultEnableIoUringNetworkEngine ? "true" : "false") << ")\n"
              << "  --no-codex           Skip Codex MCP registration\n"
              << "  --no-systemd         Skip systemd --user unit install/remove and health check\n"
              << "  --uninstall          Remove Cursor/Codex/systemd configuration\n"
              << "  --help, -h           Show this help message\n";
}

void MarkRemovedFlag(Args* args, const std::string& key, const std::string& guidance) {
    args->invalid         = true;
    args->invalid_message = "flag " + key + " is removed; " + guidance;
}

Args ParseArgs(int argc, char** argv) {
    Args args;
    args.workspace = std::filesystem::current_path().string();

    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help" || key == "-h") {
            args.show_help = true;
        } else if (key == "--uninstall") {
            args.uninstall = true;
        } else if (key == "--no-codex") {
            args.configure_codex = false;
        } else if (key == "--no-systemd") {
            args.manage_systemd = false;
        } else if (key == "--workspace" && i + 1 < argc) {
            args.workspace = argv[++i];
        } else if (key == "--url" && i + 1 < argc) {
            args.url = argv[++i];
        } else if (key == "--pgmemd-bin" && i + 1 < argc) {
            args.pgmemd_bin = argv[++i];
        } else if (key == "--pgmemd-extra-args" && i + 1 < argc) {
            args.pgmemd_extra_args = argv[++i];
        } else if (key == "--core-number" && i + 1 < argc) {
            args.core_number = std::atoi(argv[++i]);
        } else if (key == "--enable-io-uring-network-engine" && i + 1 < argc) {
            args.enable_io_uring_network_engine = ParseBoolFlag(argv[++i], args.enable_io_uring_network_engine);
        } else if (key == "--store-partitions" || key == "--allow-inmemory-fallback" ||
                   key == "--event-dispatcher-num" || key == "--write-ack-mode" ||
                   key == "--volatile-flush-interval-ms" || key == "--volatile-max-pending-ops" ||
                   key == "--shutdown-drain-timeout-ms") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "local mode keeps these options internal-only");
        } else if (key == "--store-threads") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "use --core-number instead");
        } else if (key == "--enable-io-uring") {
            if (i + 1 < argc) {
                ++i;
            }
            MarkRemovedFlag(&args, key, "io_uring storage probing is automatic in local mode");
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
        const auto resp = http.Get(pgmem::config::kDefaultHost, pgmem::config::kDefaultMcpPort, "/health", 1000);
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
    options.workspace_root                 = args.workspace;
    options.mcp_url                        = args.url;
    options.pgmemd_bin                     = args.pgmemd_bin;
    options.pgmemd_extra_args              = args.pgmemd_extra_args;
    options.core_number                    = args.core_number;
    options.enable_io_uring_network_engine = args.enable_io_uring_network_engine;
    options.configure_codex                = args.configure_codex;
    options.manage_systemd                 = args.manage_systemd;

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
            std::cerr << "pgmem install warning: service health check failed on http://" << pgmem::config::kDefaultHost
                      << ":" << pgmem::config::kDefaultMcpPort << "/health\n";
            return 2;
        }
    }

    std::cout << "pgmem install completed\n";
    return 0;
}
