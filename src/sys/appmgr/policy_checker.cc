// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/appmgr/policy_checker.h"

#include <lib/syslog/cpp/macros.h>

#include "src/sys/appmgr/allow_list.h"

namespace component {
namespace {

constexpr char kDeprecatedShellAllowList[] = "allowlist/deprecated_shell.txt";
constexpr char kDeprecatedAmbientReplaceAsExecAllowList[] =
    "allowlist/deprecated_ambient_replace_as_executable.txt";
constexpr char kComponentEventProviderAllowList[] = "allowlist/component_event_provider.txt";
constexpr char kCpuResourceAllowList[] = "allowlist/cpu_resource.txt";
constexpr char kCr50AllowList[] = "allowlist/cr50.txt";
constexpr char kDebugResourceAllowList[] = "allowlist/debug_resource.txt";
constexpr char kDurableDataAllowList[] = "allowlist/durable_data.txt";
constexpr char kFactoryDataAllowList[] = "allowlist/factory_data.txt";
constexpr char kHubAllowList[] = "allowlist/hub.txt";
constexpr char kHypervisorResourceAllowList[] = "allowlist/hypervisor_resource.txt";
constexpr char kInfoResourceAllowList[] = "allowlist/info_resource.txt";
constexpr char kIoportResourceAllowList[] = "allowlist/ioport_resource.txt";
constexpr char kIrqResourceAllowList[] = "allowlist/irq_resource.txt";
constexpr char kMmioResourceAllowList[] = "allowlist/mmio_resource.txt";
constexpr char kNnModelExecutorAllowList[] = "allowlist/nn_model_executor.txt";
constexpr char kPackageResolverAllowList[] = "allowlist/package_resolver.txt";
constexpr char kPackageCacheAllowList[] = "allowlist/package_cache.txt";
constexpr char kPkgFsVersionsAllowList[] = "allowlist/pkgfs_versions.txt";
constexpr char kPowerResourceAllowList[] = "allowlist/power_resource.txt";
constexpr char kRootJobAllowList[] = "allowlist/root_job.txt";
constexpr char kRootResourceAllowList[] = "allowlist/root_resource.txt";
constexpr char kSmcResourceAllowList[] = "allowlist/smc_resource.txt";
constexpr char kSystemUpdaterAllowList[] = "allowlist/system_updater.txt";
constexpr char kVmexResourceAllowList[] = "allowlist/vmex_resource.txt";
constexpr char kWeaveSignerAllowList[] = "allowlist/weave_signer.txt";

}  // end of namespace.

PolicyChecker::PolicyChecker(fbl::unique_fd config) : config_(std::move(config)) {}

std::optional<SecurityPolicy> PolicyChecker::Check(const SandboxMetadata& sandbox,
                                                   const FuchsiaPkgUrl& pkg_url) {
  SecurityPolicy policy;
  if (sandbox.HasService("fuchsia.sys.internal.ComponentEventProvider")) {
    if (!CheckComponentEventProvider(pkg_url)) {
      FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                     << "fuchsia.sys.internal.ComponentEventProvider";
      return std::nullopt;
    }
    policy.enable_component_event_provider = true;
  }
  if (sandbox.HasFeature("deprecated-ambient-replace-as-executable")) {
    if (!CheckDeprecatedAmbientReplaceAsExecutable(pkg_url)) {
      FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                     << "deprecated-ambient-replace-as-executable. go/fx-hermetic-sandboxes";
      return std::nullopt;
    }
    policy.enable_ambient_executable = true;
  }
  if (sandbox.HasFeature("deprecated-shell") && !CheckDeprecatedShell(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "deprecated-shell. go/fx-hermetic-sandboxes";
    return std::nullopt;
  }
  if (sandbox.HasFeature("durable-data") && !CheckDurableData(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "durable-data.";
    return std::nullopt;
  }
  if (sandbox.HasFeature("factory-data") && !CheckFactoryData(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "factory-data.";
    return std::nullopt;
  }
  if (sandbox.HasFeature("hub") && !CheckHub(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "hub. go/no-hub";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.kernel.CpuResource") && !CheckCpuResource(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.kernel.CpuResource";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.tpm.cr50.Cr50") && !CheckCr50(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.tpm.cr50.Cr50";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.kernel.DebugResource") && !CheckDebugResource(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.kernel.DebugResource";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.kernel.HypervisorResource") &&
      !CheckHypervisorResource(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.kernel.HypervisorResource";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.kernel.InfoResource") && !CheckInfoResource(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.kernel.InfoResource";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.kernel.IoportResource") && !CheckIoportResource(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.kernel.IoportResource";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.kernel.IrqResource") && !CheckIrqResource(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.kernel.IrqResource";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.kernel.MmioResource") && !CheckMmioResource(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.kernel.MmioResource";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.kernel.PowerResource") && !CheckPowerResource(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.kernel.PowerResource";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.kernel.RootJob") && !CheckRootJob(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.kernel.RootJob";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.kernel.SmcResource") && !CheckSmcResource(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.kernel.SmcResource";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.kernel.VmexResource") && !CheckVmexResource(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.kernel.VmexResource";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.nn.ModelExecutor") && !CheckNnModelExecutor(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.nn.ModelExecutor";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.weave.Signer") && !CheckWeaveSigner(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.weave.Signer";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.pkg.PackageResolver") && !CheckPackageResolver(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.pkg.PackageResolver. go/no-package-resolver";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.pkg.PackageCache") && !CheckPackageCache(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.pkg.PackageCache. go/no-package-cache";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.update.installer.Installer") && !CheckSystemUpdater(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.update.installer.Installer.";
    return std::nullopt;
  }
  if (sandbox.HasPkgFsPath("versions") && !CheckPkgFsVersions(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "pkgfs/versions. go/no-pkgfs-versions";
    return std::nullopt;
  }
  if (sandbox.HasService("fuchsia.boot.RootResource") && !CheckRootResource(pkg_url)) {
    FX_LOGS(ERROR) << "Component " << pkg_url.ToString() << " is not allowed to use "
                   << "fuchsia.boot.RootResource";
    return std::nullopt;
  }
  return policy;
}

bool PolicyChecker::CheckDeprecatedAmbientReplaceAsExecutable(const FuchsiaPkgUrl& pkg_url) {
  AllowList deprecated_exec_allowlist(config_, kDeprecatedAmbientReplaceAsExecAllowList);
  return deprecated_exec_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckComponentEventProvider(const FuchsiaPkgUrl& pkg_url) {
  AllowList component_event_provider_allowlist(config_, kComponentEventProviderAllowList);
  return component_event_provider_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckDeprecatedShell(const FuchsiaPkgUrl& pkg_url) {
  AllowList deprecated_shell_allowlist(config_, kDeprecatedShellAllowList);
  return deprecated_shell_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckDurableData(const FuchsiaPkgUrl& pkg_url) {
  AllowList durable_data_allow_list(config_, kDurableDataAllowList);
  return durable_data_allow_list.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckFactoryData(const FuchsiaPkgUrl& pkg_url) {
  AllowList factory_data_allow_list(config_, kFactoryDataAllowList);
  return factory_data_allow_list.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckHub(const FuchsiaPkgUrl& pkg_url) {
  AllowList hub_allowlist(config_, kHubAllowList);
  return hub_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckCpuResource(const FuchsiaPkgUrl& pkg_url) {
  AllowList cpu_resource_allowlist(config_, kCpuResourceAllowList);
  return cpu_resource_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckCr50(const FuchsiaPkgUrl& pkg_url) {
  AllowList cr50_allowlist(config_, kCr50AllowList);
  return cr50_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckDebugResource(const FuchsiaPkgUrl& pkg_url) {
  AllowList debug_resource_allowlist(config_, kDebugResourceAllowList);
  return debug_resource_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckHypervisorResource(const FuchsiaPkgUrl& pkg_url) {
  AllowList hypervisor_resource_allowlist(config_, kHypervisorResourceAllowList);
  return hypervisor_resource_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckInfoResource(const FuchsiaPkgUrl& pkg_url) {
  AllowList info_resource_allowlist(config_, kInfoResourceAllowList);
  return info_resource_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckIoportResource(const FuchsiaPkgUrl& pkg_url) {
  AllowList ioport_resource_allowlist(config_, kIoportResourceAllowList);
  return ioport_resource_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckIrqResource(const FuchsiaPkgUrl& pkg_url) {
  AllowList irq_resource_allowlist(config_, kIrqResourceAllowList);
  return irq_resource_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckMmioResource(const FuchsiaPkgUrl& pkg_url) {
  AllowList mmio_resource_allowlist(config_, kMmioResourceAllowList);
  return mmio_resource_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckNnModelExecutor(const FuchsiaPkgUrl& pkg_url) {
  AllowList allowlist(config_, kNnModelExecutorAllowList);
  return allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckPackageResolver(const FuchsiaPkgUrl& pkg_url) {
  AllowList package_resolver_allowlist(config_, kPackageResolverAllowList);
  return package_resolver_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckPackageCache(const FuchsiaPkgUrl& pkg_url) {
  AllowList package_cache_allowlist(config_, kPackageCacheAllowList);
  return package_cache_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckPkgFsVersions(const FuchsiaPkgUrl& pkg_url) {
  AllowList pkgfs_versions_allowlist(config_, kPkgFsVersionsAllowList);
  return pkgfs_versions_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckPowerResource(const FuchsiaPkgUrl& pkg_url) {
  AllowList power_resource_allowlist(config_, kPowerResourceAllowList);
  return power_resource_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckRootJob(const FuchsiaPkgUrl& pkg_url) {
  AllowList root_job_allowlist(config_, kRootJobAllowList);
  return root_job_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckRootResource(const FuchsiaPkgUrl& pkg_url) {
  AllowList root_resource_allowlist(config_, kRootResourceAllowList);
  return root_resource_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckSystemUpdater(const FuchsiaPkgUrl& pkg_url) {
  AllowList system_updater_allowlist(config_, kSystemUpdaterAllowList);
  return system_updater_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckSmcResource(const FuchsiaPkgUrl& pkg_url) {
  AllowList smc_resource_allowlist(config_, kSmcResourceAllowList);
  return smc_resource_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckVmexResource(const FuchsiaPkgUrl& pkg_url) {
  AllowList vmex_resource_allowlist(config_, kVmexResourceAllowList);
  return vmex_resource_allowlist.IsAllowed(pkg_url);
}

bool PolicyChecker::CheckWeaveSigner(const FuchsiaPkgUrl& pkg_url) {
  AllowList weave_signer_allowlist(config_, kWeaveSignerAllowList);
  return weave_signer_allowlist.IsAllowed(pkg_url);
}

}  // namespace component
