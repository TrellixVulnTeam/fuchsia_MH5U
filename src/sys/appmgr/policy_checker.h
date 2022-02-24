// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_APPMGR_POLICY_CHECKER_H_
#define SRC_SYS_APPMGR_POLICY_CHECKER_H_

#include <optional>
#include <string>

#include <fbl/unique_fd.h>

#include "gtest/gtest_prod.h"
#include "src/lib/cmx/sandbox.h"
#include "src/lib/pkg_url/fuchsia_pkg_url.h"

namespace component {

// Holds the list of policies that are returned by the policy checker. These are
// used by the Realm to correctly setup the environment.
struct SecurityPolicy {
  bool enable_ambient_executable = false;
  bool enable_component_event_provider = false;
};

// The job of the `PolicyChecker` is to enforce that security policies placed
// on the sandbox are enforced at runtime. For example if a component attempts
// to enable ambient executability within its component manifiest but is not on
// a specific allowlist defined in `//src/security/policy` this object will
// catch it.
class PolicyChecker final {
 public:
  explicit PolicyChecker(fbl::unique_fd config);
  // Returns a Policy object if the check was successful else no policy could
  // be set due to a policy being violated. If nullopt is returned the
  // component should not be launched.
  std::optional<SecurityPolicy> Check(const SandboxMetadata& sandbox, const FuchsiaPkgUrl& pkg_url);

 private:
  fbl::unique_fd config_;

  bool CheckDeprecatedShell(const FuchsiaPkgUrl& pkg_url);
  bool CheckDeprecatedAmbientReplaceAsExecutable(const FuchsiaPkgUrl& pkg_url);
  bool CheckDurableData(const FuchsiaPkgUrl& pkg_url);
  bool CheckFactoryData(const FuchsiaPkgUrl& pkg_url);
  bool CheckComponentEventProvider(const FuchsiaPkgUrl& pkg_url);
  bool CheckCpuResource(const FuchsiaPkgUrl& pkg_url);
  bool CheckCr50(const FuchsiaPkgUrl& pkg_url);
  bool CheckDebugResource(const FuchsiaPkgUrl& pkg_url);
  bool CheckHub(const FuchsiaPkgUrl& pkg_url);
  bool CheckHypervisorResource(const FuchsiaPkgUrl& pkg_url);
  bool CheckInfoResource(const FuchsiaPkgUrl& pkg_url);
  bool CheckIoportResource(const FuchsiaPkgUrl& pkg_url);
  bool CheckIrqResource(const FuchsiaPkgUrl& pkg_url);
  bool CheckMmioResource(const FuchsiaPkgUrl& pkg_url);
  bool CheckNnModelExecutor(const FuchsiaPkgUrl& pkg_url);
  bool CheckPackageResolver(const FuchsiaPkgUrl& pkg_url);
  bool CheckPackageCache(const FuchsiaPkgUrl& pkg_url);
  bool CheckPkgFsVersions(const FuchsiaPkgUrl& pkg_url);
  bool CheckPowerResource(const FuchsiaPkgUrl& pkg_url);
  bool CheckRootJob(const FuchsiaPkgUrl& pkg_url);
  bool CheckRootResource(const FuchsiaPkgUrl& pkg_url);
  bool CheckSmcResource(const FuchsiaPkgUrl& pkg_url);
  bool CheckSystemUpdater(const FuchsiaPkgUrl& pkg_url);
  bool CheckVmexResource(const FuchsiaPkgUrl& pkg_url);
  bool CheckWeaveSigner(const FuchsiaPkgUrl& pkg_url);

  FRIEND_TEST(PolicyCheckerTest, ReplaceAsExecPolicyPresent);
  FRIEND_TEST(PolicyCheckerTest, ReplaceAsExecPolicyAbsent);
  FRIEND_TEST(PolicyCheckerTest, CpuResourcePolicy);
  FRIEND_TEST(PolicyCheckerTest, Cr50Policy);
  FRIEND_TEST(PolicyCheckerTest, DebugResourcePolicy);
  FRIEND_TEST(PolicyCheckerTest, HubPolicy);
  FRIEND_TEST(PolicyCheckerTest, HypervisorResourcePolicy);
  FRIEND_TEST(PolicyCheckerTest, InfoResourcePolicy);
  FRIEND_TEST(PolicyCheckerTest, IoportResourcePolicy);
  FRIEND_TEST(PolicyCheckerTest, IrqResourcePolicy);
  FRIEND_TEST(PolicyCheckerTest, MmioResourcePolicy);
  FRIEND_TEST(PolicyCheckerTest, NnExecutorModelPolicy);
  FRIEND_TEST(PolicyCheckerTest, PackageResolverPolicy);
  FRIEND_TEST(PolicyCheckerTest, PackageCachePolicy);
  FRIEND_TEST(PolicyCheckerTest, PkgFsVersionsPolicy);
  FRIEND_TEST(PolicyCheckerTest, PowerResourcePolicy);
  FRIEND_TEST(PolicyCheckerTest, RootJobPolicy);
  FRIEND_TEST(PolicyCheckerTest, RootResourcePolicy);
  FRIEND_TEST(PolicyCheckerTest, SmcResourcePolicy);
  FRIEND_TEST(PolicyCheckerTest, SystemUpdaterPolicy);
  FRIEND_TEST(PolicyCheckerTest, VmexResourcePolicy);
  FRIEND_TEST(PolicyCheckerTest, WeaveSignerPolicy);
};

}  // end of namespace component.

#endif  // SRC_SYS_APPMGR_POLICY_CHECKER_H_
