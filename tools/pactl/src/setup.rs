use crate::build::BuildOptions;
use crate::config::WorkspaceConfig;
use crate::disk::{ensure_disk_image, DiskEnsureMode, DiskEnsureOutputs};
use crate::kernel::{build_kernel, KernelBuildOutputs};
use crate::run::invalidate_run_cache;
use crate::sync::{
    prepare_sync_inputs, sync_bootfs_generated, sync_rootfs_generated, BootfsSyncOutputs,
    RootfsSyncOutputs,
};
use std::path::Path;

#[derive(Copy, Clone)]
pub enum SetupMode {
    Diff,
    Full,
}

pub struct SetupOutputs {
    pub mode: SetupMode,
    pub kernel: KernelBuildOutputs,
    pub disk: DiskEnsureOutputs,
    pub bootfs: BootfsSyncOutputs,
    pub rootfs: RootfsSyncOutputs,
}

pub fn setup_workspace(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    mode: SetupMode,
) -> Result<SetupOutputs, String> {
    let kernel = build_kernel(workspace_root, workspace)?;
    let disk = ensure_disk_image(
        workspace_root,
        workspace,
        match mode {
            SetupMode::Diff => DiskEnsureMode::IfMissing,
            SetupMode::Full => DiskEnsureMode::Always,
        },
    )?;
    let inputs = prepare_sync_inputs(workspace_root, workspace, BuildOptions::default())?;
    let bootfs = sync_bootfs_generated(
        workspace_root,
        workspace,
        &inputs.disk_image,
        &inputs.manifests,
    )?;
    let rootfs = sync_rootfs_generated(
        workspace_root,
        workspace,
        &inputs.disk_image,
        &inputs.manifests,
    )?;
    invalidate_run_cache(workspace_root)?;

    Ok(SetupOutputs {
        mode,
        kernel,
        disk,
        bootfs,
        rootfs,
    })
}
