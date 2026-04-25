use crate::build::{build_userland, planned_artifact_path, BuildOptions};
use crate::config::{discover_apps, DiskPartition, WorkspaceConfig};
use crate::host_tools::{ensure_host_tool, HostTool};
use crate::manifest::{generate_manifests, GeneratedManifestPaths};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

pub struct RootfsSyncOutputs {
    pub disk_image: PathBuf,
    pub manifest: PathBuf,
}

pub struct BootfsSyncOutputs {
    pub disk_image: PathBuf,
    pub bootfs_image: PathBuf,
    pub bootfs_manifest: PathBuf,
    pub esp_manifest: PathBuf,
}

pub struct PreparedSyncInputs {
    pub disk_image: PathBuf,
    pub manifests: GeneratedManifestPaths,
}

struct ManifestPair {
    image_path: String,
    source_path: String,
}

pub fn sync_rootfs(workspace_root: &Path, workspace: &WorkspaceConfig) -> Result<RootfsSyncOutputs, String> {
    let inputs = prepare_sync_inputs(workspace_root, workspace, BuildOptions::default())?;
    sync_rootfs_generated(workspace_root, workspace, &inputs.disk_image, &inputs.manifests)
}

pub fn sync_bootfs(workspace_root: &Path, workspace: &WorkspaceConfig) -> Result<BootfsSyncOutputs, String> {
    let inputs = prepare_sync_inputs(workspace_root, workspace, BuildOptions::default())?;
    sync_bootfs_generated(workspace_root, workspace, &inputs.disk_image, &inputs.manifests)
}

pub fn prepare_sync_inputs(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    build_options: BuildOptions,
) -> Result<PreparedSyncInputs, String> {
    let disk_image = workspace_root.join(&workspace.disk.image);
    require_nonempty_file(
        &disk_image,
        "disk image",
        "run pactl disk ensure or create the disk image before syncing",
    )?;

    build_userland(workspace_root, workspace, None, build_options)?;
    let manifests = generate_manifests(workspace_root, workspace)?;
    Ok(PreparedSyncInputs { disk_image, manifests })
}

pub fn sync_rootfs_generated(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    disk_image: &Path,
    manifests: &GeneratedManifestPaths,
) -> Result<RootfsSyncOutputs, String> {
    let rootfs_builder = ensure_host_tool(workspace_root, workspace, HostTool::RootfsBuilder)?;
    let rootfs_partition = find_partition(workspace, "rootfs")?;

    let mut cmd = Command::new(&rootfs_builder);
    cmd.current_dir(workspace_root);
    cmd.arg(disk_image);
    cmd.arg(rootfs_partition.index.to_string());
    cmd.arg(&manifests.rootfs);
    run_command("sync rootfs", &mut cmd)?;

    Ok(RootfsSyncOutputs {
        disk_image: disk_image.to_path_buf(),
        manifest: manifests.rootfs.clone(),
    })
}

pub fn sync_bootfs_generated(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    disk_image: &Path,
    manifests: &GeneratedManifestPaths,
) -> Result<BootfsSyncOutputs, String> {
    let bootfs_pairs = load_manifest_pairs(&manifests.bootfs, false)?;
    let shell_source = bootfs_pairs
        .iter()
        .find(|entry| entry.image_path == "/cmd/shell.elf")
        .map(|entry| PathBuf::from(&entry.source_path))
        .ok_or_else(|| format!("bootfs manifest is missing /cmd/shell.elf: {}", manifests.bootfs.display()))?;

    let bootfs_image = workspace_root
        .join(&workspace.artifacts.dir)
        .join("bootfs")
        .join("BOOTFS.IMG");
    if let Some(parent) = bootfs_image.parent() {
        fs::create_dir_all(parent)
            .map_err(|err| format!("failed to create {}: {err}", parent.display()))?;
    }

    let bootfs_builder = ensure_host_tool(workspace_root, workspace, HostTool::BootfsBuilder)?;
    let mut bootfs_cmd = Command::new(&bootfs_builder);
    bootfs_cmd.current_dir(workspace_root);
    bootfs_cmd.arg(&bootfs_image);
    for entry in &bootfs_pairs {
        bootfs_cmd.arg(&entry.image_path);
        bootfs_cmd.arg(&entry.source_path);
    }
    run_command("build bootfs image", &mut bootfs_cmd)?;

    let esp_manifest = generate_esp_manifest(workspace_root, workspace, &bootfs_image, &shell_source)?;
    let esp_builder = ensure_host_tool(workspace_root, workspace, HostTool::EspBuilder)?;
    let esp_partition = find_partition(workspace, "esp")?;

    let mut esp_cmd = Command::new(&esp_builder);
    esp_cmd.current_dir(workspace_root);
    esp_cmd.arg(&disk_image);
    esp_cmd.arg(esp_partition.index.to_string());
    esp_cmd.arg(&esp_manifest);
    run_command("sync bootfs/esp", &mut esp_cmd)?;

    Ok(BootfsSyncOutputs {
        disk_image: disk_image.to_path_buf(),
        bootfs_image,
        bootfs_manifest: manifests.bootfs.clone(),
        esp_manifest,
    })
}

fn find_partition<'a>(workspace: &'a WorkspaceConfig, partition_id: &str) -> Result<&'a DiskPartition, String> {
    workspace
        .disk
        .partitions
        .iter()
        .find(|partition| partition.id == partition_id)
        .ok_or_else(|| format!("missing [[disk.partition]] id = '{}'", partition_id))
}

fn load_manifest_pairs(manifest_path: &Path, allow_dirs: bool) -> Result<Vec<ManifestPair>, String> {
    let contents = fs::read_to_string(manifest_path)
        .map_err(|err| format!("failed to read {}: {err}", manifest_path.display()))?;
    let mut pairs = Vec::new();

    for (line_index, raw_line) in contents.lines().enumerate() {
        let line_number = line_index + 1;
        let line = raw_line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some((image_path, source_path)) = line.split_once('=') else {
            return Err(format!("{}:{line_number}: invalid manifest line", manifest_path.display()));
        };
        let image_path = image_path.trim();
        let source_path = source_path.trim();
        if image_path.is_empty() || source_path.is_empty() {
            return Err(format!("{}:{line_number}: invalid manifest line", manifest_path.display()));
        }
        if source_path == "@dir" {
            if allow_dirs {
                continue;
            }
            return Err(format!(
                "{}:{line_number}: directory entries are not supported here ({})",
                manifest_path.display(),
                image_path
            ));
        }
        pairs.push(ManifestPair {
            image_path: image_path.to_string(),
            source_path: source_path.to_string(),
        });
    }

    Ok(pairs)
}

fn generate_esp_manifest(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    bootfs_image: &Path,
    shell_source: &Path,
) -> Result<PathBuf, String> {
    let apps = discover_apps(workspace_root, workspace)?;
    let init_image = workspace_root
        .join(&workspace.kernel.dir)
        .join("zig-out")
        .join("bin")
        .join("EFI")
        .join("BOOT")
        .join("INITAPP.ELF");
    let bootx64 = workspace_root
        .join(&workspace.kernel.dir)
        .join("zig-out")
        .join("bin")
        .join("EFI")
        .join("BOOT")
        .join("BOOTX64.EFI");

    require_nonempty_file(&bootx64, "EFI boot image", "run zig build efi in kernel first")?;
    require_nonempty_file(&init_image, "init image", "run zig build efi in kernel first")?;

    let shell_output = if let Some(shell_app) = apps.iter().find(|app| app.app.id == "shell") {
        let output_path = planned_artifact_path(workspace_root, workspace, shell_app);
        require_nonempty_file(&output_path, "shell image", "run pactl build userland first")?;
        output_path
    } else {
        require_nonempty_file(shell_source, "shell image", "run pactl build userland first")?;
        shell_source.to_path_buf()
    };

    require_nonempty_file(bootfs_image, "bootfs image", "run pactl sync bootfs again after userland build")?;

    let manifest_path = workspace_root
        .join(&workspace.manifests.dir)
        .join("esp.generated.txt");
    if let Some(parent) = manifest_path.parent() {
        fs::create_dir_all(parent)
            .map_err(|err| format!("failed to create {}: {err}", parent.display()))?;
    }

    let contents = format!(
        concat!(
            "# Generated by pactl. DO NOT EDIT.\n",
            "/EFI/BOOT/BOOTX64.EFI={}\n",
            "/EFI/BOOT/SHELL.ELF={}\n",
            "/EFI/BOOT/INITAPP.ELF={}\n",
            "/EFI/BOOT/BOOTFS.IMG={}\n"
        ),
        bootx64.display(),
        shell_output.display(),
        init_image.display(),
        bootfs_image.display()
    );
    write_if_changed(&manifest_path, &contents)?;
    Ok(manifest_path)
}

fn require_nonempty_file(path: &Path, label: &str, hint: &str) -> Result<(), String> {
    let metadata = fs::metadata(path)
        .map_err(|_| format!("missing {}: {} ({})", label, path.display(), hint))?;
    if !metadata.is_file() || metadata.len() == 0 {
        return Err(format!("empty {}: {} ({})", label, path.display(), hint));
    }
    Ok(())
}

fn write_if_changed(path: &Path, contents: &str) -> Result<(), String> {
    if let Ok(existing) = fs::read_to_string(path) {
        if existing == contents {
            return Ok(());
        }
    }
    fs::write(path, contents).map_err(|err| format!("failed to write {}: {err}", path.display()))
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
