#include <string>

#include "pgmem/install/workspace_config.h"
#include "test_framework.h"

TEST_CASE(test_runtime_installer_no_systemd_idempotent) {
    pgmem::install::InstallOptions options;
    options.manage_systemd = false;
    options.store_root     = "/tmp/pgmem-runtime-installer-test";

    pgmem::install::WorkspaceConfigurator configurator;
    std::string error;

    ASSERT_TRUE(configurator.Install(options, &error));
    ASSERT_TRUE(error.empty());
    ASSERT_TRUE(configurator.Install(options, &error));
    ASSERT_TRUE(error.empty());

    ASSERT_TRUE(configurator.Uninstall(options, &error));
    ASSERT_TRUE(error.empty());
    ASSERT_TRUE(configurator.Uninstall(options, &error));
    ASSERT_TRUE(error.empty());
}
