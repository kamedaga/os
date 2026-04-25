use crate::config::WorkspaceConfig;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::SystemTime;

#[derive(Copy, Clone)]
pub enum HostTool {
    BootfsBuilder,
    RootfsBuilder,
    EspBuilder,
}

impl HostTool {
    fn name(self) -> &'static str {
        match self {
            Self::BootfsBuilder => "bootfs_builder",
            Self::RootfsBuilder => "rootfs_builder",
            Self::EspBuilder => "esp_builder",
        }
    }

    fn output_file_name(self) -> String {
        format!("{}{}", self.name(), env::consts::EXE_SUFFIX)
    }

    fn source_paths(self) -> &'static [&'static str] {
        match self {
            Self::BootfsBuilder => &["tools/bootfs_builder.zig"],
            Self::RootfsBuilder | Self::EspBuilder => &[
                "tools/rootfs_host.zig",
                "userland/support/persistent_fs_layout.zig",
            ],
        }
    }
}

pub fn ensure_host_tool(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    tool: HostTool,
) -> Result<PathBuf, String> {
    let output_dir = workspace_root.join(&workspace.artifacts.dir).join("host-tools");
    fs::create_dir_all(&output_dir)
        .map_err(|err| format!("failed to create {}: {err}", output_dir.display()))?;
    let output_path = output_dir.join(tool.output_file_name());

    if !needs_rebuild(workspace_root, &output_path, tool)? {
        return Ok(output_path);
    }

    let zig = if workspace.toolchain.zig.is_empty() {
        "zig"
    } else {
        workspace.toolchain.zig.as_str()
    };
    let cache_root = workspace_root.join(&workspace.artifacts.dir).join("zig-cache");
    let local_cache = cache_root.join("local");
    let global_cache = cache_root.join("global");
    fs::create_dir_all(&local_cache)
        .map_err(|err| format!("failed to create {}: {err}", local_cache.display()))?;
    fs::create_dir_all(&global_cache)
        .map_err(|err| format!("failed to create {}: {err}", global_cache.display()))?;

    let mut cmd = Command::new(zig);
    cmd.current_dir(workspace_root);
    cmd.arg("build-exe");
    cmd.arg("--name").arg(tool.name());
    cmd.arg("-O").arg("ReleaseSmall");
    match tool {
        HostTool::BootfsBuilder => {
            cmd.arg("tools/bootfs_builder.zig");
        }
        HostTool::RootfsBuilder => {
            cmd.arg("--dep").arg("rootfs_host");
            cmd.arg("-Mroot=tools/rootfs_builder.zig");
            cmd.arg("--dep").arg("persistent_fs_layout");
            cmd.arg("-Mrootfs_host=tools/rootfs_host.zig");
            cmd.arg("-Mpersistent_fs_layout=userland/support/persistent_fs_layout.zig");
        }
        HostTool::EspBuilder => {
            cmd.arg("--dep").arg("rootfs_host");
            cmd.arg("-Mroot=tools/esp_builder.zig");
            cmd.arg("--dep").arg("persistent_fs_layout");
            cmd.arg("-Mrootfs_host=tools/rootfs_host.zig");
            cmd.arg("-Mpersistent_fs_layout=userland/support/persistent_fs_layout.zig");
        }
    }
    cmd.arg(workspace_path_flag(
        workspace_root,
        "-femit-bin=",
        &output_path,
    ));
    cmd.arg("--cache-dir")
        .arg(workspace_path(workspace_root, &local_cache));
    cmd.arg("--global-cache-dir")
        .arg(workspace_path(workspace_root, &global_cache));

    run_command(&format!("build host tool {}", tool.name()), &mut cmd)?;
    Ok(output_path)
}

fn needs_rebuild(workspace_root: &Path, output_path: &Path, tool: HostTool) -> Result<bool, String> {
    let output_time = match fs::metadata(output_path).and_then(|meta| meta.modified()) {
        Ok(time) => time,
        Err(_) => return Ok(true),
    };

    let mut source_paths = vec![match tool {
        HostTool::BootfsBuilder => "tools/bootfs_builder.zig",
        HostTool::RootfsBuilder => "tools/rootfs_builder.zig",
        HostTool::EspBuilder => "tools/esp_builder.zig",
    }];
    source_paths.extend(tool.source_paths());

    for relative_path in source_paths {
        let source_path = workspace_root.join(relative_path);
        let source_time = modified_time(&source_path)?;
        if source_time > output_time {
            return Ok(true);
        }
    }
    Ok(false)
}

fn modified_time(path: &Path) -> Result<SystemTime, String> {
    fs::metadata(path)
        .and_then(|meta| meta.modified())
        .map_err(|err| format!("failed to read modified time for {}: {err}", path.display()))
}

fn workspace_path(path_root: &Path, path: &Path) -> String {
    if let Ok(relative) = path.strip_prefix(path_root) {
        relative.display().to_string().replace('\\', "/")
    } else {
        path.display().to_string().replace('\\', "/")
    }
}

fn workspace_path_flag(path_root: &Path, prefix: &str, path: &Path) -> String {
    format!("{prefix}{}", workspace_path(path_root, path))
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
