use crate::config::WorkspaceConfig;
use std::path::{Path, PathBuf};
use std::process::Command;

pub struct KernelBuildOutputs {
    pub build_dir: PathBuf,
    pub step: String,
}

pub fn build_kernel(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
) -> Result<KernelBuildOutputs, String> {
    let zig = if workspace.toolchain.zig.is_empty() {
        "zig"
    } else {
        workspace.toolchain.zig.as_str()
    };
    let kernel_dir = workspace_root.join(&workspace.kernel.dir);
    let mut cmd = Command::new(zig);
    cmd.current_dir(&kernel_dir);
    cmd.arg("build");
    cmd.arg(&workspace.kernel.default_step);
    run_command("build kernel", &mut cmd)?;

    Ok(KernelBuildOutputs {
        build_dir: kernel_dir,
        step: workspace.kernel.default_step.clone(),
    })
}

fn run_command(label: &str, cmd: &mut Command) -> Result<(), String> {
    let debug = format!("{cmd:?}");
    eprintln!("pactl: running {label}: {debug}");
    let status = cmd
        .status()
        .map_err(|err| format!("failed to run {label}: {err}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!(
            "{label} failed with exit code {:?}: {debug}",
            status.code()
        ))
    }
}
