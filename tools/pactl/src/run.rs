use crate::build::planned_artifact_path;
use crate::config::{discover_apps, WorkspaceConfig};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::SystemTime;

const OVMF_CODE_PATH: &str = "/usr/share/OVMF/OVMF_CODE_4M.fd";
const OVMF_VARS_TEMPLATE_PATH: &str = "/usr/share/OVMF/OVMF_VARS_4M.fd";

pub struct RunOptions {
    pub timed: bool,
    pub kvm: bool,
    pub dry_run: bool,
}

pub struct RunPlan {
    pub disk_image: PathBuf,
    pub ovmf_vars: PathBuf,
    pub qemu_log: PathBuf,
    pub serial_log: Option<PathBuf>,
    pub summary_log: Option<PathBuf>,
    pub script: String,
}

pub fn run_qemu(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    options: &RunOptions,
) -> Result<RunPlan, String> {
    let artifact_dir = workspace_root.join(&workspace.artifacts.dir);
    fs::create_dir_all(&artifact_dir)
        .map_err(|err| format!("failed to create {}: {err}", artifact_dir.display()))?;

    let disk_image = workspace_root.join(&workspace.disk.image);
    let ovmf_vars = artifact_dir.join("OVMF_VARS.fd");
    let qemu_log = artifact_dir.join("qemu.log");
    let serial_log = options.timed.then(|| artifact_dir.join("serial-timed.log"));
    let summary_log = options
        .timed
        .then(|| artifact_dir.join("boot-timing-summary.txt"));

    validate_run_inputs(workspace_root, workspace, &disk_image)?;
    let script = build_wsl_script(
        workspace_root,
        &disk_image,
        &ovmf_vars,
        &qemu_log,
        serial_log.as_deref(),
        summary_log.as_deref(),
        options,
    )?;

    if !options.dry_run {
        let status = Command::new("wsl")
            .arg("-e")
            .arg("bash")
            .arg("-lc")
            .arg(&script)
            .status()
            .map_err(|err| format!("failed to launch WSL QEMU command: {err}"))?;
        if !status.success() {
            return Err(format!("QEMU run failed with exit code {:?}", status.code()));
        }
    }

    Ok(RunPlan {
        disk_image,
        ovmf_vars,
        qemu_log,
        serial_log,
        summary_log,
        script,
    })
}

fn validate_run_inputs(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    disk_image: &Path,
) -> Result<(), String> {
    require_nonempty_file(
        disk_image,
        "disk image",
        "run pactl sync rootfs and pactl sync bootfs first",
    )?;

    let bootx64 = workspace_root
        .join(&workspace.kernel.dir)
        .join("zig-out")
        .join("bin")
        .join("EFI")
        .join("BOOT")
        .join("BOOTX64.EFI");
    let initapp = workspace_root
        .join(&workspace.kernel.dir)
        .join("zig-out")
        .join("bin")
        .join("EFI")
        .join("BOOT")
        .join("INITAPP.ELF");
    let bootfs_image = workspace_root
        .join(&workspace.artifacts.dir)
        .join("bootfs")
        .join("BOOTFS.IMG");

    require_nonempty_file(&bootx64, "EFI boot image", "run zig build efi first")?;
    require_nonempty_file(&initapp, "init image", "run zig build efi first")?;
    require_nonempty_file(&bootfs_image, "bootfs image", "run pactl sync bootfs first")?;

    let disk_time = modified_time(disk_image)?;
    for stale_path in stale_candidates(workspace_root, workspace, &bootx64, &initapp, &bootfs_image)? {
        if let Ok(path_time) = modified_time(&stale_path) {
            if path_time > disk_time {
                return Err(format!(
                    "stale disk image detected:\n  {} is newer than {}\nrun pactl sync bootfs and pactl sync rootfs to refresh the disk image",
                    stale_path.display(),
                    disk_image.display()
                ));
            }
        }
    }

    Ok(())
}

fn stale_candidates(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    bootx64: &Path,
    initapp: &Path,
    bootfs_image: &Path,
) -> Result<Vec<PathBuf>, String> {
    let mut paths = vec![bootx64.to_path_buf(), initapp.to_path_buf(), bootfs_image.to_path_buf()];
    for app in discover_apps(workspace_root, workspace)? {
        let artifact = planned_artifact_path(workspace_root, workspace, &app);
        if artifact.exists() {
            paths.push(artifact);
        }
    }
    Ok(paths)
}

fn build_wsl_script(
    workspace_root: &Path,
    disk_image: &Path,
    ovmf_vars: &Path,
    qemu_log: &Path,
    serial_log: Option<&Path>,
    summary_log: Option<&Path>,
    options: &RunOptions,
) -> Result<String, String> {
    let workspace_wsl = windows_path_to_wsl(workspace_root)?;
    let disk_wsl = windows_path_to_wsl(disk_image)?;
    let ovmf_vars_wsl = windows_path_to_wsl(ovmf_vars)?;
    let qemu_log_wsl = windows_path_to_wsl(qemu_log)?;
    let artifact_dir_wsl = windows_path_to_wsl(
        ovmf_vars
            .parent()
            .ok_or_else(|| format!("missing artifact directory for {}", ovmf_vars.display()))?,
    )?;
    let serial_log_wsl = serial_log.map(windows_path_to_wsl).transpose()?;
    let summary_log_wsl = summary_log.map(windows_path_to_wsl).transpose()?;
    let timestamp_stream = windows_path_to_wsl(&workspace_root.join("tools/timestamp_stream.py"))?;
    let summarize_script =
        windows_path_to_wsl(&workspace_root.join("tools/summarize_boot_timed_log.py"))?;

    let mut qemu_parts = vec![
        "qemu-system-x86_64".to_string(),
        "-machine q35".to_string(),
        "-m 512M".to_string(),
        "-monitor none".to_string(),
        "-d int,guest_errors,cpu_reset".to_string(),
        format!("-D {}", bash_quote(&qemu_log_wsl)),
        "-display gtk,grab-on-hover=off".to_string(),
        "-vga none".to_string(),
        "-device virtio-vga".to_string(),
        "-device virtio-tablet-pci".to_string(),
        "-device virtio-keyboard-pci".to_string(),
        format!(
            "-drive if=pflash,format=raw,readonly=on,file={}",
            bash_quote(OVMF_CODE_PATH)
        ),
        format!(
            "-drive if=pflash,format=raw,file={}",
            bash_quote(&ovmf_vars_wsl)
        ),
        format!(
            "-drive if=none,file={},format=raw,id=bootdisk",
            bash_quote(&disk_wsl)
        ),
        "-device virtio-blk-pci,drive=bootdisk".to_string(),
        "-serial stdio".to_string(),
    ];
    if options.kvm {
        qemu_parts.insert(1, "-cpu host".to_string());
        qemu_parts.insert(1, "-enable-kvm".to_string());
    }
    let qemu_cmd = qemu_parts.join(" \\\n  ");

    let mut script = String::new();
    script.push_str("set -euo pipefail\n");
    script.push_str(&format!("cd {}\n", bash_quote(&workspace_wsl)));
    script.push_str(&format!("ARTIFACT_DIR={}\n", bash_quote(&artifact_dir_wsl)));
    script.push_str(&format!("DISK_IMG={}\n", bash_quote(&disk_wsl)));
    script.push_str(&format!("OVMF_VARS={}\n", bash_quote(&ovmf_vars_wsl)));
    script.push_str(&format!("QEMU_LOG={}\n", bash_quote(&qemu_log_wsl)));
    if let Some(serial_log_wsl) = &serial_log_wsl {
        script.push_str(&format!("SERIAL_LOG={}\n", bash_quote(serial_log_wsl)));
    }
    if let Some(summary_log_wsl) = &summary_log_wsl {
        script.push_str(&format!("SUMMARY_LOG={}\n", bash_quote(summary_log_wsl)));
    }
    script.push_str("mkdir -p \"$ARTIFACT_DIR\"\n");
    if options.timed {
        script.push_str("if ! command -v python3 >/dev/null 2>&1; then echo 'missing python3'; exit 1; fi\n");
    }
    script.push_str("rm -f \"$OVMF_VARS\" \"$QEMU_LOG\"");
    if options.timed {
        script.push_str(" \"$SERIAL_LOG\" \"$SUMMARY_LOG\"");
    }
    script.push('\n');
    script.push_str(&format!(
        "cp {} \"$OVMF_VARS\"\n",
        bash_quote(OVMF_VARS_TEMPLATE_PATH)
    ));

    if options.timed {
        script.push_str("set +e\n");
        script.push_str(&qemu_cmd);
        script.push_str(&format!(
            " | python3 {} | tee \"$SERIAL_LOG\"\n",
            bash_quote(&timestamp_stream)
        ));
        script.push_str("qemu_status=${PIPESTATUS[0]}\n");
        script.push_str("set -e\n");
        script.push_str(&format!(
            "python3 {} \"$SERIAL_LOG\" | tee \"$SUMMARY_LOG\"\n",
            bash_quote(&summarize_script)
        ));
        script.push_str("echo\n");
        script.push_str("echo \"serial log: $SERIAL_LOG\"\n");
        script.push_str("echo \"summary: $SUMMARY_LOG\"\n");
        script.push_str("exit \"$qemu_status\"\n");
    } else {
        script.push_str(&qemu_cmd);
        script.push('\n');
    }

    Ok(script)
}

fn require_nonempty_file(path: &Path, label: &str, hint: &str) -> Result<(), String> {
    let metadata = fs::metadata(path)
        .map_err(|_| format!("missing {}: {} ({})", label, path.display(), hint))?;
    if !metadata.is_file() || metadata.len() == 0 {
        return Err(format!("empty {}: {} ({})", label, path.display(), hint));
    }
    Ok(())
}

fn modified_time(path: &Path) -> Result<SystemTime, String> {
    fs::metadata(path)
        .and_then(|meta| meta.modified())
        .map_err(|err| format!("failed to read modified time for {}: {err}", path.display()))
}

fn windows_path_to_wsl(path: &Path) -> Result<String, String> {
    let mut path_str = path
        .canonicalize()
        .unwrap_or_else(|_| path.to_path_buf())
        .display()
        .to_string()
        .replace('\\', "/");
    if let Some(stripped) = path_str.strip_prefix("//?/") {
        path_str = stripped.to_string();
    }
    if let Some(stripped) = path_str.strip_prefix("\\\\?\\") {
        path_str = stripped.replace('\\', "/");
    }
    let bytes = path_str.as_bytes();
    if bytes.len() >= 3 && bytes[1] == b':' && bytes[2] == b'/' {
        let drive = (bytes[0] as char).to_ascii_lowercase();
        let rest = &path_str[3..];
        Ok(format!("/mnt/{drive}/{rest}"))
    } else if path_str.starts_with('/') {
        Ok(path_str)
    } else {
        Err(format!("cannot convert path to WSL path: {}", path.display()))
    }
}

fn bash_quote(value: &str) -> String {
    let escaped = value.replace('\'', "'\"'\"'");
    format!("'{escaped}'")
}
