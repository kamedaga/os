use crate::build::planned_artifact_path;
use crate::config::{discover_apps, WorkspaceConfig};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::SystemTime;

const OVMF_CODE_PATH: &str = "/usr/share/OVMF/OVMF_CODE_4M.fd";
const OVMF_VARS_TEMPLATE_PATH: &str = "/usr/share/OVMF/OVMF_VARS_4M.fd";
const QEMU_DEBUG_FLAGS: &str = "guest_errors,cpu_reset";

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

pub fn invalidate_run_cache(workspace_root: &Path) -> Result<(), String> {
    let slug = runtime_slug(workspace_root);
    let script = format!(
        "set -e\nCACHE_DIR=${{XDG_CACHE_HOME:-$HOME/.cache}}/capabilityos-qemu/{slug}\nrm -f \"$CACHE_DIR/disk.dirty\" \"$CACHE_DIR/disk.meta\" \"$CACHE_DIR/disk.img\"\n"
    );
    let status = Command::new("wsl")
        .arg("-e")
        .arg("bash")
        .arg("-lc")
        .arg(&script)
        .status()
        .map_err(|err| format!("failed to invalidate WSL run cache: {err}"))?;
    if !status.success() {
        return Err(format!(
            "failed to invalidate WSL run cache for {}: exit code {:?}",
            workspace_root.display(),
            status.code()
        ));
    }
    Ok(())
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
            return Err(format!(
                "QEMU run failed with exit code {:?}",
                status.code()
            ));
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
    for stale_path in
        stale_candidates(workspace_root, workspace, &bootx64, &initapp, &bootfs_image)?
    {
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
    let mut paths = vec![
        bootx64.to_path_buf(),
        initapp.to_path_buf(),
        bootfs_image.to_path_buf(),
    ];
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
    let artifact_dir_wsl = windows_path_to_wsl(
        ovmf_vars
            .parent()
            .ok_or_else(|| format!("missing artifact directory for {}", ovmf_vars.display()))?,
    )?;
    let ovmf_vars_wsl = windows_path_to_wsl(ovmf_vars)?;
    let qemu_log_wsl = windows_path_to_wsl(qemu_log)?;
    let serial_log_wsl = serial_log.map(windows_path_to_wsl).transpose()?;
    let summary_log_wsl = summary_log.map(windows_path_to_wsl).transpose()?;
    let timestamp_stream = windows_path_to_wsl(&workspace_root.join("tools/timestamp_stream.py"))?;
    let summarize_script =
        windows_path_to_wsl(&workspace_root.join("tools/summarize_boot_timed_log.py"))?;
    let launcher_script = windows_path_to_wsl(&workspace_root.join("tools/wsl_launcher.py"))?;
    let runtime_dir_wsl = format!("/tmp/capabilityos-qemu-{}", runtime_slug(workspace_root));
    let launcher_socket_wsl = format!("{runtime_dir_wsl}/launcher.sock");
    let launcher_log_wsl = format!("{runtime_dir_wsl}/launcher.log");
    let runtime_ovmf_vars_wsl = format!("{runtime_dir_wsl}/OVMF_VARS.fd");
    let runtime_qemu_log_wsl = format!("{runtime_dir_wsl}/qemu.log");
    let runtime_serial_log_wsl = serial_log
        .as_ref()
        .map(|_| format!("{runtime_dir_wsl}/serial-timed.log"));
    let runtime_summary_log_wsl = summary_log
        .as_ref()
        .map(|_| format!("{runtime_dir_wsl}/boot-timing-summary.txt"));
    let cache_dir_wsl = format!(
        "${{XDG_CACHE_HOME:-$HOME/.cache}}/capabilityos-qemu/{}",
        runtime_slug(workspace_root)
    );

    let mut qemu_parts = vec![
        "qemu-system-x86_64".to_string(),
        "-machine q35".to_string(),
        "-m 512M".to_string(),
        "-monitor none".to_string(),
        format!("-d {QEMU_DEBUG_FLAGS}"),
        format!("-D {}", bash_quote(&runtime_qemu_log_wsl)),
        "-display gtk,gl=on,grab-on-hover=off".to_string(),
        "-vga none".to_string(),
        "-device virtio-vga-gl,xres=1920,yres=1080".to_string(),
        "-device virtio-tablet-pci".to_string(),
        "-device virtio-keyboard-pci".to_string(),
        format!(
            "-drive if=pflash,format=raw,readonly=on,file={}",
            bash_quote(OVMF_CODE_PATH)
        ),
        format!(
            "-drive if=pflash,format=raw,file={}",
            bash_quote(&runtime_ovmf_vars_wsl)
        ),
        "-drive if=none,file=\"$CACHE_DISK\",format=raw,id=bootdisk".to_string(),
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
    script.push_str(&format!("RUNTIME_DIR={}\n", bash_quote(&runtime_dir_wsl)));
    script.push_str(&format!(
        "LAUNCHER_SOCKET={}\n",
        bash_quote(&launcher_socket_wsl)
    ));
    script.push_str(&format!("LAUNCHER_LOG={}\n", bash_quote(&launcher_log_wsl)));
    script.push_str(&format!("DISK_IMG={}\n", bash_quote(&disk_wsl)));
    script.push_str(&format!("OVMF_VARS={}\n", bash_quote(&ovmf_vars_wsl)));
    script.push_str(&format!("QEMU_LOG={}\n", bash_quote(&qemu_log_wsl)));
    script.push_str(&format!(
        "RUNTIME_OVMF_VARS={}\n",
        bash_quote(&runtime_ovmf_vars_wsl)
    ));
    script.push_str(&format!(
        "RUNTIME_QEMU_LOG={}\n",
        bash_quote(&runtime_qemu_log_wsl)
    ));
    if let Some(serial_log_wsl) = &serial_log_wsl {
        script.push_str(&format!("SERIAL_LOG={}\n", bash_quote(serial_log_wsl)));
    }
    if let Some(summary_log_wsl) = &summary_log_wsl {
        script.push_str(&format!("SUMMARY_LOG={}\n", bash_quote(summary_log_wsl)));
    }
    if let Some(runtime_serial_log_wsl) = &runtime_serial_log_wsl {
        script.push_str(&format!(
            "RUNTIME_SERIAL_LOG={}\n",
            bash_quote(runtime_serial_log_wsl)
        ));
    }
    if let Some(runtime_summary_log_wsl) = &runtime_summary_log_wsl {
        script.push_str(&format!(
            "RUNTIME_SUMMARY_LOG={}\n",
            bash_quote(runtime_summary_log_wsl)
        ));
    }
    script.push_str(&format!("CACHE_DIR={cache_dir_wsl}\n"));
    script.push_str("CACHE_LOCK=\"$CACHE_DIR/run.lock\"\n");
    script.push_str("CACHE_DISK=\"$CACHE_DIR/disk.img\"\n");
    script.push_str("CACHE_META=\"$CACHE_DIR/disk.meta\"\n");
    script.push_str("CACHE_DIRTY=\"$CACHE_DIR/disk.dirty\"\n");
    script.push_str("mkdir -p \"$ARTIFACT_DIR\"\n");
    script.push_str("mkdir -p \"$RUNTIME_DIR\"\n");
    script.push_str("mkdir -p \"$CACHE_DIR\"\n");
    script.push_str(
        "if ! command -v python3 >/dev/null 2>&1; then echo 'missing python3'; exit 1; fi\n",
    );
    script
        .push_str("if ! command -v flock >/dev/null 2>&1; then echo 'missing flock'; exit 1; fi\n");
    script.push_str(&format!(
        "python3 {} ensure --socket \"$LAUNCHER_SOCKET\" --log \"$LAUNCHER_LOG\"\n",
        bash_quote(&launcher_script)
    ));
    script.push_str("source_disk_sig() {\n");
    script.push_str("  stat -c '%s:%Y' \"$DISK_IMG\"\n");
    script.push_str("}\n");
    script.push_str("cached_disk_sig() {\n");
    script.push_str("  [ -f \"$CACHE_META\" ] && cat \"$CACHE_META\"\n");
    script.push_str("}\n");
    script.push_str("refresh_cache_disk() {\n");
    script.push_str("  cp \"$DISK_IMG\" \"$CACHE_DISK.tmp\"\n");
    script.push_str("  mv \"$CACHE_DISK.tmp\" \"$CACHE_DISK\"\n");
    script.push_str("  source_disk_sig > \"$CACHE_META\"\n");
    script.push_str("  rm -f \"$CACHE_DIRTY\"\n");
    script.push_str("}\n");
    script.push_str("schedule_writeback() {\n");
    script.push_str("  : > \"$CACHE_DIRTY\"\n");
    script.push_str("  flock -u 9\n");
    script.push_str(&format!(
        "  python3 {} spawn-writeback --lock \"$CACHE_LOCK\" --source \"$CACHE_DISK\" --dest \"$DISK_IMG\" --meta \"$CACHE_META\" --dirty \"$CACHE_DIRTY\" --log \"$LAUNCHER_LOG\"\n",
        bash_quote(&launcher_script)
    ));
    script.push_str("}\n");
    script.push_str("sync_logs() {\n");
    script.push_str(
        "  [ ! -f \"$RUNTIME_OVMF_VARS\" ] || cp \"$RUNTIME_OVMF_VARS\" \"$OVMF_VARS\"\n",
    );
    script.push_str("  [ ! -f \"$RUNTIME_QEMU_LOG\" ] || cp \"$RUNTIME_QEMU_LOG\" \"$QEMU_LOG\"\n");
    if options.timed {
        script.push_str(
            "  [ ! -f \"$RUNTIME_SERIAL_LOG\" ] || cp \"$RUNTIME_SERIAL_LOG\" \"$SERIAL_LOG\"\n",
        );
        script.push_str(
            "  [ ! -f \"$RUNTIME_SUMMARY_LOG\" ] || cp \"$RUNTIME_SUMMARY_LOG\" \"$SUMMARY_LOG\"\n",
        );
    }
    script.push_str("}\n");
    script.push_str("trap sync_logs EXIT\n");
    script.push_str("exec 9>\"$CACHE_LOCK\"\n");
    script.push_str("flock 9\n");
    script.push_str(
        "rm -f \"$OVMF_VARS\" \"$QEMU_LOG\" \"$RUNTIME_OVMF_VARS\" \"$RUNTIME_QEMU_LOG\"",
    );
    if options.timed {
        script.push_str(
            " \"$SERIAL_LOG\" \"$SUMMARY_LOG\" \"$RUNTIME_SERIAL_LOG\" \"$RUNTIME_SUMMARY_LOG\"",
        );
    }
    script.push('\n');
    script.push_str("if [ -f \"$CACHE_DIRTY\" ]; then\n");
    script.push_str("  if [ \"$(cached_disk_sig)\" != \"$(source_disk_sig)\" ]; then\n");
    script.push_str(
        "    echo 'disk cache is dirty and the Windows disk image changed outside WSL'\n",
    );
    script.push_str("    echo 'resolve the disk image divergence before running again'\n");
    script.push_str("    exit 1\n");
    script.push_str("  fi\n");
    script.push_str("elif [ ! -f \"$CACHE_DISK\" ] || [ \"$(cached_disk_sig)\" != \"$(source_disk_sig)\" ]; then\n");
    script.push_str("  refresh_cache_disk\n");
    script.push_str("fi\n");
    script.push_str(&format!(
        "cp {} \"$RUNTIME_OVMF_VARS\"\n",
        bash_quote(OVMF_VARS_TEMPLATE_PATH)
    ));

    if options.timed {
        script.push_str("set +e\n");
        script.push_str(&qemu_cmd);
        script.push_str(&format!(
            " | python3 {} | tee \"$RUNTIME_SERIAL_LOG\"\n",
            bash_quote(&timestamp_stream)
        ));
        script.push_str("qemu_status=${PIPESTATUS[0]}\n");
        script.push_str("set -e\n");
        script.push_str("schedule_writeback\n");
        script.push_str(&format!(
            "python3 {} \"$RUNTIME_SERIAL_LOG\" | tee \"$RUNTIME_SUMMARY_LOG\"\n",
            bash_quote(&summarize_script)
        ));
        script.push_str("echo\n");
        script.push_str("echo \"serial log: $SERIAL_LOG\"\n");
        script.push_str("echo \"summary: $SUMMARY_LOG\"\n");
        script.push_str("exit \"$qemu_status\"\n");
    } else {
        script.push_str("set +e\n");
        script.push_str(&qemu_cmd);
        script.push('\n');
        script.push_str("qemu_status=$?\n");
        script.push_str("set -e\n");
        script.push_str("schedule_writeback\n");
        script.push_str("exit \"$qemu_status\"\n");
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
        Err(format!(
            "cannot convert path to WSL path: {}",
            path.display()
        ))
    }
}

fn bash_quote(value: &str) -> String {
    let escaped = value.replace('\'', "'\"'\"'");
    format!("'{escaped}'")
}

fn runtime_slug(path: &Path) -> String {
    let mut slug = String::with_capacity(64);
    for ch in path
        .canonicalize()
        .unwrap_or_else(|_| path.to_path_buf())
        .display()
        .to_string()
        .chars()
    {
        if ch.is_ascii_alphanumeric() {
            slug.push(ch.to_ascii_lowercase());
        } else if slug.is_empty() || !slug.ends_with('-') {
            slug.push('-');
        }
    }
    slug.trim_matches('-').to_string()
}
