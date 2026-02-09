#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "pgmem/install/workspace_config.h"
#include "pgmem/net/http_client.h"

namespace {

struct Args {
    bool uninstall{false};
    bool configure_codex{true};
    bool manage_systemd{true};
    bool show_help{false};
    std::string workspace;
    std::string url{"http://127.0.0.1:8765/mcp"};
    std::string pgmemd_bin{"pgmemd"};
};

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  pgmem-install [options]\n\n"
        << "Options:\n"
        << "  --workspace <path>   Workspace root to configure (default: current directory)\n"
        << "  --url <mcp-url>      MCP URL written to .cursor/mcp.json (default: http://127.0.0.1:8765/mcp)\n"
        << "  --pgmemd-bin <path>  pgmemd binary path for systemd unit (default: pgmemd)\n"
        << "  --no-codex           Skip Codex MCP registration\n"
        << "  --no-systemd         Skip systemd --user unit install/remove and health check\n"
        << "  --uninstall          Remove Cursor/Codex/systemd configuration\n"
        << "  --help, -h           Show this help message\n";
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
        }
    }

    return args;
}

bool HealthCheck() {
    pgmem::net::HttpClient http;
    for (int i = 0; i < 5; ++i) {
        const auto resp = http.Get("127.0.0.1", 8765, "/health", 1000);
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
    if (args.show_help) {
        PrintUsage();
        return 0;
    }

    pgmem::install::InstallOptions options;
    options.workspace_root = args.workspace;
    options.mcp_url = args.url;
    options.pgmemd_bin = args.pgmemd_bin;
    options.configure_codex = args.configure_codex;
    options.manage_systemd = args.manage_systemd;

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
            std::cerr << "pgmem install warning: service health check failed on http://127.0.0.1:8765/health\n";
            return 2;
        }
    }

    std::cout << "pgmem install completed\n";
    return 0;
}
