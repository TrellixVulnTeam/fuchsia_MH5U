// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

use {
    anyhow::{Context, Error},
    fidl_fidl_examples_echo as fidl_echo,
    fuchsia_async::{self as fasync},
    fuchsia_component::client::{launch, launcher},
    lazy_static::lazy_static,
};

macro_rules! policy_url {
    ($cmx:expr) => {
        format!("{}{}", "fuchsia-pkg://fuchsia.com/policy-integration-tests#meta/", $cmx)
    };
}

lazy_static! {
    static ref NONE_ACCEPTED_URL: String = policy_url!("none.cmx");
    static ref PACKAGE_CACHE_DENIED_URL: String = policy_url!("package_cache_denied.cmx");
    static ref PACKAGE_CACHE_ALLOWED_URL: String = policy_url!("package_cache_allowed.cmx");
    static ref PACKAGE_RESOLVER_DENIED_URL: String = policy_url!("package_resolver_denied.cmx");
    static ref PACKAGE_RESOLVER_ALLOWED_URL: String = policy_url!("package_resolver_allowed.cmx");
    static ref ROOT_JOB_DENIED_URL: String = policy_url!("root_job_denied.cmx");
    static ref ROOT_JOB_ALLOWED_URL: String = policy_url!("root_job_allowed.cmx");
    static ref CPU_RESOURCE_DENIED_URL: String = policy_url!("cpu_resource_denied.cmx");
    static ref CPU_RESOURCE_ALLOWED_URL: String = policy_url!("cpu_resource_allowed.cmx");
    static ref DEBUG_RESOURCE_DENIED_URL: String = policy_url!("debug_resource_denied.cmx");
    static ref DEBUG_RESOURCE_ALLOWED_URL: String = policy_url!("debug_resource_allowed.cmx");
    static ref HYPERVISOR_RESOURCE_DENIED_URL: String =
        policy_url!("hypervisor_resource_denied.cmx");
    static ref HYPERVISOR_RESOURCE_ALLOWED_URL: String =
        policy_url!("hypervisor_resource_allowed.cmx");
    static ref MMIO_RESOURCE_DENIED_URL: String = policy_url!("mmio_resource_denied.cmx");
    static ref MMIO_RESOURCE_ALLOWED_URL: String = policy_url!("mmio_resource_allowed.cmx");
    static ref INFO_RESOURCE_DENIED_URL: String = policy_url!("info_resource_denied.cmx");
    static ref INFO_RESOURCE_ALLOWED_URL: String = policy_url!("info_resource_allowed.cmx");
    static ref IRQ_RESOURCE_DENIED_URL: String = policy_url!("irq_resource_denied.cmx");
    static ref IRQ_RESOURCE_ALLOWED_URL: String = policy_url!("irq_resource_allowed.cmx");
    static ref IOPORT_RESOURCE_DENIED_URL: String = policy_url!("ioport_resource_denied.cmx");
    static ref IOPORT_RESOURCE_ALLOWED_URL: String = policy_url!("ioport_resource_allowed.cmx");
    static ref POWER_RESOURCE_DENIED_URL: String = policy_url!("power_resource_denied.cmx");
    static ref POWER_RESOURCE_ALLOWED_URL: String = policy_url!("power_resource_allowed.cmx");
    static ref SMC_RESOURCE_DENIED_URL: String = policy_url!("smc_resource_denied.cmx");
    static ref SMC_RESOURCE_ALLOWED_URL: String = policy_url!("smc_resource_allowed.cmx");
    static ref ROOT_RESOURCE_DENIED_URL: String = policy_url!("root_resource_denied.cmx");
    static ref ROOT_RESOURCE_ALLOWED_URL: String = policy_url!("root_resource_allowed.cmx");
    static ref VMEX_RESOURCE_DENIED_URL: String = policy_url!("vmex_resource_denied.cmx");
    static ref VMEX_RESOURCE_ALLOWED_URL: String = policy_url!("vmex_resource_allowed.cmx");
    static ref PKGFS_VERSIONS_DENIED_URL: String = policy_url!("pkgfs_versions_denied.cmx");
    static ref PKGFS_VERSIONS_ALLOWED_URL: String = policy_url!("pkgfs_versions_allowed.cmx");
    static ref DEPRECATED_SHELL_DENIED_URL: String = policy_url!("deprecated_shell_denied.cmx");
    static ref DEPRECATED_SHELL_ALLOWED_URL: String = policy_url!("deprecated_shell_allowed.cmx");
    static ref DEPRECATED_EXEC_DENIED_URL: String =
        policy_url!("deprecated_ambient_replace_as_exec_denied.cmx");
    static ref DEPRECATED_EXEC_ALLOWED_URL: String =
        policy_url!("deprecated_ambient_replace_as_exec_allowed.cmx");
}

async fn launch_component(component_url: &str) -> Result<String, Error> {
    let launcher = launcher().context("failed to open the launcher")?;
    let app =
        launch(&launcher, component_url.to_string(), None).context("failed to launch service")?;
    let echo = app
        .connect_to_protocol::<fidl_echo::EchoMarker>()
        .context("Failed to connect to echo service")?;
    let result = echo.echo_string(Some("policy")).await?;
    Ok(result.unwrap())
}

async fn assert_launch_allowed(component_url: &str) {
    assert!(launch_component(component_url).await.unwrap() == "policy")
}

async fn assert_launch_denied(component_url: &str) {
    assert!(launch_component(component_url).await.is_err())
}

#[fasync::run_singlethreaded(test)]
async fn none_allowed() -> Result<(), Error> {
    assert_launch_allowed(&NONE_ACCEPTED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn package_cache_allowed() -> Result<(), Error> {
    assert_launch_allowed(&PACKAGE_CACHE_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn package_cache_denied() -> Result<(), Error> {
    assert_launch_denied(&PACKAGE_CACHE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn package_resolver_allowed() -> Result<(), Error> {
    assert_launch_allowed(&PACKAGE_RESOLVER_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn package_resolver_denied() -> Result<(), Error> {
    assert_launch_denied(&PACKAGE_RESOLVER_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn root_job_allowed() -> Result<(), Error> {
    assert_launch_allowed(&ROOT_JOB_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn root_job_denied() -> Result<(), Error> {
    assert_launch_denied(&ROOT_JOB_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn cpu_resource_allowed() -> Result<(), Error> {
    assert_launch_allowed(&CPU_RESOURCE_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn cpu_resource_denied() -> Result<(), Error> {
    assert_launch_denied(&CPU_RESOURCE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn debug_resource_allowed() -> Result<(), Error> {
    assert_launch_allowed(&DEBUG_RESOURCE_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn debug_resource_denied() -> Result<(), Error> {
    assert_launch_denied(&DEBUG_RESOURCE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn hypervisor_resource_allowed() -> Result<(), Error> {
    assert_launch_allowed(&HYPERVISOR_RESOURCE_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn hypervisor_resource_denied() -> Result<(), Error> {
    assert_launch_denied(&HYPERVISOR_RESOURCE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn mmio_resource_allowed() -> Result<(), Error> {
    assert_launch_allowed(&MMIO_RESOURCE_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn mmio_resource_denied() -> Result<(), Error> {
    assert_launch_denied(&MMIO_RESOURCE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn info_resource_allowed() -> Result<(), Error> {
    assert_launch_allowed(&INFO_RESOURCE_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn info_resource_denied() -> Result<(), Error> {
    assert_launch_denied(&INFO_RESOURCE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn irq_resource_allowed() -> Result<(), Error> {
    assert_launch_allowed(&IRQ_RESOURCE_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn irq_resource_denied() -> Result<(), Error> {
    assert_launch_denied(&IRQ_RESOURCE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn ioport_resource_allowed() -> Result<(), Error> {
    assert_launch_allowed(&IOPORT_RESOURCE_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn ioport_resource_denied() -> Result<(), Error> {
    assert_launch_denied(&IOPORT_RESOURCE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn power_resource_allowed() -> Result<(), Error> {
    assert_launch_allowed(&POWER_RESOURCE_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn power_resource_denied() -> Result<(), Error> {
    assert_launch_denied(&POWER_RESOURCE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn smc_resource_allowed() -> Result<(), Error> {
    assert_launch_allowed(&SMC_RESOURCE_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn smc_resource_denied() -> Result<(), Error> {
    assert_launch_denied(&SMC_RESOURCE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn root_resource_allowed() -> Result<(), Error> {
    assert_launch_allowed(&ROOT_RESOURCE_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn root_resource_denied() -> Result<(), Error> {
    assert_launch_denied(&ROOT_RESOURCE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn vmex_resource_allowed() -> Result<(), Error> {
    assert_launch_allowed(&VMEX_RESOURCE_ALLOWED_URL).await;
    Ok(())
}

// The `vmex_resource_denied` test here is disabled since eng builds have a permissive * allowlist
// for `VmexResource` to support out-of-tree tests that need to be able to JIT without having to
// enumerate every single test component in-tree that e.g. Chromium wants to run from out-of-tree.
// Unfortunately, this means that it's impossible to make a component that will be denied
// `VmexResource` on eng builds, which means we can't test this behavior using the approach
// taken in this testsuite, so we simply disable this test for the time being.
// TODO(https://fxbug.dev/78074): enable this test or something exercising equivalent coverage
#[ignore]
#[fasync::run_singlethreaded(test)]
async fn vmex_resource_denied() -> Result<(), Error> {
    assert_launch_denied(&VMEX_RESOURCE_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn pkgfs_versions_allowed() -> Result<(), Error> {
    assert_launch_allowed(&PKGFS_VERSIONS_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn pkgfs_versions_denied() -> Result<(), Error> {
    assert_launch_denied(&PKGFS_VERSIONS_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn deprecated_shell_allowed() -> Result<(), Error> {
    assert_launch_allowed(&DEPRECATED_SHELL_ALLOWED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn deprecated_shell_denied() -> Result<(), Error> {
    assert_launch_denied(&DEPRECATED_SHELL_DENIED_URL).await;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn deprecated_exec_allowed() -> Result<(), Error> {
    assert_launch_allowed(&DEPRECATED_EXEC_ALLOWED_URL).await;
    Ok(())
}

// Disabled because we can't reasonably test this on eng builds, because we need to be permissive
// to allow tests that use JITs to run.  See similar discussion around `vmex_resource_denied`.
// TODO(https://fxbug.dev/78074): enable this test or something exercising equivalent coverage
#[ignore]
#[fasync::run_singlethreaded(test)]
async fn deprecated_exec_denied() -> Result<(), Error> {
    assert_launch_denied(&DEPRECATED_EXEC_DENIED_URL).await;
    Ok(())
}
