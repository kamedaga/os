use crate::build::planned_artifact_path;
use crate::config::{app_is_skipped, discover_apps, WorkspaceConfig};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::SystemTime;

const OVMF_CODE_PATH: &str = "/usr/share/OVMF/OVMF_CODE_4M.fd";
const OVMF_VARS_TEMPLATE_PATH: &str = "/usr/share/OVMF/OVMF_VARS_4M.fd";
const QEMU_DEBUG_FLAGS: &str = "guest_errors,cpu_reset";
const TIMED_RUN_SECONDS: u32 = 30;
const PF_CHECK_RUN_SECONDS: u32 = 60;

pub struct RunOptions {
    pub timed: bool,
    pub kvm: bool,
    pub dry_run: bool,
    pub pf_check_jobs: usize,
    pub console: ConsoleBackend,
    pub display: DisplayBackend,
    pub split_windows: bool,
}

#[derive(Copy, Clone, Eq, PartialEq)]
pub enum ConsoleBackend {
    Off,
    Pty,
    Stdio,
}

#[derive(Copy, Clone, Eq, PartialEq)]
pub enum DisplayBackend {
    Gtk,
    None,
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
    if options.console == ConsoleBackend::Stdio && (options.timed || options.pf_check_jobs != 0) {
        return Err("--console=stdio cannot be combined with timed or pf-check runs because those consume stdio for the serial log".to_string());
    }
    if options.split_windows {
        if options.console != ConsoleBackend::Pty {
            return Err("--split-windows requires --console=pty".to_string());
        }
        if options.timed || options.pf_check_jobs != 0 {
            return Err(
                "--split-windows cannot be combined with timed or pf-check runs".to_string(),
            );
        }
    }
    let artifact_dir = workspace_root.join(&workspace.artifacts.dir);
    fs::create_dir_all(&artifact_dir)
        .map_err(|err| format!("failed to create {}: {err}", artifact_dir.display()))?;

    let disk_image = workspace_root.join(&workspace.disk.image);
    let ovmf_vars = artifact_dir.join("OVMF_VARS.fd");
    let qemu_log = artifact_dir.join("qemu.log");
    let serial_log = if options.timed {
        Some(artifact_dir.join("serial-timed.log"))
    } else {
        None
    };
    let summary_log = options
        .timed
        .then(|| artifact_dir.join("boot-timing-summary.txt"));

    validate_run_inputs(workspace_root, workspace, &disk_image)?;

    if options.pf_check_jobs != 0 {
        let summary_log = artifact_dir.join("pf-check").join("summary.txt");
        let script =
            build_wsl_pf_check_script(workspace_root, &artifact_dir, &disk_image, options)?;
        if !options.dry_run {
            let status = Command::new("wsl")
                .arg("-e")
                .arg("bash")
                .arg("-lc")
                .arg(&script)
                .status()
                .map_err(|err| format!("failed to launch WSL QEMU PF check: {err}"))?;
            if !status.success() {
                if !pf_check_summary_reports_success(&summary_log) {
                    return Err(format!(
                        "parallel PF check failed with exit code {:?}",
                        status.code()
                    ));
                }
            }
        }
        return Ok(RunPlan {
            disk_image,
            ovmf_vars,
            qemu_log,
            serial_log: None,
            summary_log: Some(summary_log),
            script,
        });
    }

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
            if options.timed
                && summary_log
                    .as_ref()
                    .is_some_and(|path| timed_summary_reports_success(path))
            {
                return Ok(RunPlan {
                    disk_image,
                    ovmf_vars,
                    qemu_log,
                    serial_log,
                    summary_log,
                    script,
                });
            }
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

fn timed_summary_reports_success(path: &Path) -> bool {
    fs::read_to_string(path)
        .map(|summary| summary.contains("interactive boot complete"))
        .unwrap_or(false)
}

fn pf_check_summary_reports_success(path: &Path) -> bool {
    fs::read_to_string(path)
        .map(|summary| {
            summary.contains("parallel PF check ok:")
                && !summary.contains("smoke summary incomplete")
                && !summary.contains("PF marker found")
                && !summary.contains("reset loop after boot start")
                && !summary.contains("qemu failed")
                && !summary.contains("qemu exited before smoke completed")
        })
        .unwrap_or(false)
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
    let kernel_initapp = workspace_root
        .join(&workspace.kernel.dir)
        .join("zig-out")
        .join("bin")
        .join("EFI")
        .join("BOOT")
        .join("INITAPP.ELF");
    let initapp = discover_apps(workspace_root, workspace)?
        .iter()
        .find(|app| app.app.id == "seed2_boot" && !app_is_skipped(workspace, app))
        .map(|app| planned_artifact_path(workspace_root, workspace, app))
        .unwrap_or(kernel_initapp);
    let bootfs_image = workspace_root
        .join(&workspace.artifacts.dir)
        .join("bootfs")
        .join("BOOTFS.IMG");

    require_nonempty_file(&bootx64, "EFI boot image", "run zig build efi first")?;
    require_nonempty_file(&initapp, "init image", "run pactl setup first")?;
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

    let serial_backend = if options.console == ConsoleBackend::Stdio {
        "-serial file:$RUNTIME_DIR/serial.log".to_string()
    } else {
        "-serial stdio".to_string()
    };
    let mut qemu_parts = vec![
        "qemu-system-x86_64".to_string(),
        "-machine q35".to_string(),
        "-smp 4".to_string(),
        "-m 2G".to_string(),
        "-no-reboot".to_string(),
        "-monitor none".to_string(),
        format!("-d {QEMU_DEBUG_FLAGS}"),
        format!("-D {}", bash_quote(&runtime_qemu_log_wsl)),
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
        serial_backend,
    ];
    append_display_devices(&mut qemu_parts, options.display);
    append_console_device(&mut qemu_parts, options.console);
    append_network_device(&mut qemu_parts);
    if options.kvm {
        qemu_parts.insert(1, "-cpu host".to_string());
        qemu_parts.insert(1, "-enable-kvm".to_string());
    }
    let qemu_cmd = qemu_parts.join(" \\\n  ");

    let mut script = String::new();
    script.push_str("set -euo pipefail\n");
    script.push_str(&format!("cd {}\n", bash_quote(&workspace_wsl)));
    script.push_str(&format!("ARTIFACT_DIR={}\n", bash_quote(&artifact_dir_wsl)));
    script.push_str(&format!("RUNTIME_DIR={runtime_dir_wsl}\n"));
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
    if options.split_windows {
        script.push_str("cat > \"$RUNTIME_DIR/attach-console.sh\" <<'CAPCONSOLE_ATTACH'\n");
        script.push_str("#!/usr/bin/env bash\n");
        script.push_str("set -euo pipefail\n");
        script.push_str("pty=\"$1\"\n");
        script.push_str("echo \"CapabilityOS console: $pty\"\n");
        script.push_str("exec 3<>\"$pty\"\n");
        script.push_str("old_stty=\"\"\n");
        script.push_str("old_pty_stty=$(stty -g < \"$pty\" 2>/dev/null || true)\n");
        script.push_str("if [ -t 0 ]; then old_stty=$(stty -g < /dev/tty 2>/dev/null || true); stty raw -echo < /dev/tty 2>/dev/null || true; fi\n");
        script.push_str("stty raw -echo < \"$pty\" 2>/dev/null || true\n");
        script.push_str("cleanup() { if [ -n \"$old_stty\" ]; then stty \"$old_stty\" < /dev/tty 2>/dev/null || true; fi; if [ -n \"$old_pty_stty\" ]; then stty \"$old_pty_stty\" < \"$pty\" 2>/dev/null || true; fi; }\n");
        script.push_str("trap cleanup EXIT\n");
        script.push_str("cat <&3 & reader=$!\n");
        script.push_str("cat >&3 || true\n");
        script.push_str("echo \"CapabilityOS console input closed; keeping output attached until QEMU exits.\"\n");
        script.push_str("wait \"$reader\" 2>/dev/null || true\n");
        script.push_str("CAPCONSOLE_ATTACH\n");
        script.push_str("chmod +x \"$RUNTIME_DIR/attach-console.sh\"\n");
        script.push_str("launch_console_window() {\n");
        script.push_str("  pty=\"$1\"\n");
        script.push_str("  echo \"console pty: $pty\"\n");
        script.push_str("  if command -v cmd.exe >/dev/null 2>&1; then\n");
        script.push_str("    cmd.exe /C start \"\" wt.exe -w -1 new-tab --title CapabilityOS-console wsl.exe -e bash \"$RUNTIME_DIR/attach-console.sh\" \"$pty\" >/dev/null 2>&1 && return 0\n");
        script.push_str("    cmd.exe /C start \"\" powershell.exe -NoExit -Command wsl.exe -e bash \"$RUNTIME_DIR/attach-console.sh\" \"$pty\" >/dev/null 2>&1 && return 0\n");
        script.push_str("  fi\n");
        script.push_str("  if command -v wt.exe >/dev/null 2>&1; then\n");
        script.push_str("    wt.exe -w -1 new-tab --title CapabilityOS-console wsl.exe -e bash \"$RUNTIME_DIR/attach-console.sh\" \"$pty\" >/dev/null 2>&1 && return 0\n");
        script.push_str("  fi\n");
        script.push_str("  if command -v powershell.exe >/dev/null 2>&1; then\n");
        script.push_str("    powershell.exe -NoExit -Command wsl.exe -e bash \"$RUNTIME_DIR/attach-console.sh\" \"$pty\" >/dev/null 2>&1 && return 0\n");
        script.push_str("  fi\n");
        script.push_str("  echo \"open another terminal and run: wsl -e bash $RUNTIME_DIR/attach-console.sh $pty\"\n");
        script.push_str("}\n");
        script.push_str("split_windows_qemu() {\n");
        script.push_str("  opened=0\n");
        script.push_str("  while IFS= read -r line; do\n");
        script.push_str("    echo \"$line\"\n");
        script.push_str("    case \"$line\" in\n");
        script.push_str("      *\"char device redirected to \"*)\n");
        script.push_str("        if [ \"$opened\" -eq 0 ]; then\n");
        script.push_str("          rest=${line#*char device redirected to }\n");
        script.push_str("          pty=${rest%% *}\n");
        script.push_str("          launch_console_window \"$pty\"\n");
        script.push_str("          opened=1\n");
        script.push_str("        fi\n");
        script.push_str("        ;;\n");
        script.push_str("    esac\n");
        script.push_str("  done\n");
        script.push_str("}\n");
    }
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
    }
    if options.timed {
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
        script.push_str(" \"$SERIAL_LOG\" \"$RUNTIME_SERIAL_LOG\"");
    }
    if options.timed {
        script.push_str(" \"$SUMMARY_LOG\" \"$RUNTIME_SUMMARY_LOG\"");
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
        script.push_str(&format!(
            "timeout --kill-after=2s {TIMED_RUN_SECONDS}s \\\n  "
        ));
        script.push_str(&qemu_cmd);
        script.push_str(&format!(
            " | python3 {} | tee \"$RUNTIME_SERIAL_LOG\"\n",
            bash_quote(&timestamp_stream)
        ));
        script.push_str("qemu_status=${PIPESTATUS[0]}\n");
        script.push_str("set -e\n");
        script.push_str("if [ \"$qemu_status\" -eq 124 ] || [ \"$qemu_status\" -eq 137 ]; then\n");
        script.push_str("  sync_logs\n");
        script.push_str("  set +e\n");
        script.push_str(&format!(
            "  python3 {} --check \"$RUNTIME_SERIAL_LOG\" | tee \"$RUNTIME_SUMMARY_LOG\"\n",
            bash_quote(&summarize_script)
        ));
        script.push_str("  summary_status=${PIPESTATUS[0]}\n");
        script.push_str("  set -e\n");
        script.push_str("  sync_logs\n");
        script.push_str("  echo\n");
        script.push_str("  echo \"serial log: $SERIAL_LOG\"\n");
        script.push_str("  echo \"summary: $SUMMARY_LOG\"\n");
        script.push_str("  if [ \"$summary_status\" -eq 0 ]; then\n");
        script.push_str("    echo 'timed run reached interactive boot success before timeout'\n");
        script.push_str("    trap - EXIT\n");
        script.push_str("    exit 0\n");
        script.push_str("  fi\n");
        script.push_str(&format!(
            "  echo \"timed run stopped after {TIMED_RUN_SECONDS}s; treating QEMU timeout as captured failure\"\n"
        ));
        script.push_str("  exit \"$qemu_status\"\n");
        script.push_str("fi\n");
        script.push_str("schedule_writeback\n");
        script.push_str("set +e\n");
        script.push_str(&format!(
            "python3 {} --check \"$RUNTIME_SERIAL_LOG\" | tee \"$RUNTIME_SUMMARY_LOG\"\n",
            bash_quote(&summarize_script)
        ));
        script.push_str("summary_status=${PIPESTATUS[0]}\n");
        script.push_str("set -e\n");
        script.push_str("echo\n");
        script.push_str("echo \"serial log: $SERIAL_LOG\"\n");
        script.push_str("echo \"summary: $SUMMARY_LOG\"\n");
        script.push_str("if [ \"$summary_status\" -eq 0 ] && { [ \"$qemu_status\" -eq 0 ] || [ \"$qemu_status\" -eq 1 ]; }; then\n");
        script.push_str("  echo 'timed run reached interactive boot success'\n");
        script.push_str("  trap - EXIT\n");
        script.push_str("  exit 0\n");
        script.push_str("fi\n");
        script.push_str("if [ \"$qemu_status\" -eq 0 ]; then\n");
        script.push_str("  exit \"$summary_status\"\n");
        script.push_str("fi\n");
        script.push_str("exit \"$qemu_status\"\n");
    } else {
        script.push_str("set +e\n");
        if options.split_windows {
            script.push_str(&qemu_cmd);
            script.push_str(" 2>&1 | split_windows_qemu\n");
            script.push_str("qemu_status=${PIPESTATUS[0]}\n");
        } else {
            script.push_str(&qemu_cmd);
            script.push('\n');
            script.push_str("qemu_status=$?\n");
        }
        script.push_str("set -e\n");
        script.push_str("schedule_writeback\n");
        script.push_str("exit \"$qemu_status\"\n");
    }

    Ok(script)
}

fn append_display_devices(qemu_parts: &mut Vec<String>, backend: DisplayBackend) {
    match backend {
        DisplayBackend::Gtk => {
            qemu_parts.push("-display gtk,gl=on,grab-on-hover=off".to_string());
            qemu_parts.push("-vga none".to_string());
            qemu_parts.push("-device virtio-vga-gl,xres=1920,yres=1080".to_string());
            qemu_parts.push("-device virtio-tablet-pci".to_string());
            qemu_parts.push("-device virtio-keyboard-pci".to_string());
        }
        DisplayBackend::None => {
            qemu_parts.push("-display none".to_string());
        }
    }
}

fn append_console_device(qemu_parts: &mut Vec<String>, backend: ConsoleBackend) {
    match backend {
        ConsoleBackend::Off => {}
        ConsoleBackend::Pty => {
            qemu_parts.push("-device virtio-serial-pci".to_string());
            qemu_parts.push("-chardev pty,id=capconsole".to_string());
            qemu_parts.push(
                "-device virtconsole,chardev=capconsole,name=capabilityos.console.0".to_string(),
            );
        }
        ConsoleBackend::Stdio => {
            qemu_parts.push("-device virtio-serial-pci".to_string());
            qemu_parts.push("-chardev stdio,id=capconsole,signal=off".to_string());
            qemu_parts.push(
                "-device virtconsole,chardev=capconsole,name=capabilityos.console.0".to_string(),
            );
        }
    }
}

fn append_network_device(qemu_parts: &mut Vec<String>) {
    qemu_parts.push("-netdev user,id=capnet0,ipv6=off,dhcpstart=10.0.2.15".to_string());
    qemu_parts.push("-device virtio-net-pci,netdev=capnet0,mac=52:54:00:12:34:56".to_string());
}

fn build_wsl_pf_check_script(
    workspace_root: &Path,
    artifact_root: &Path,
    disk_image: &Path,
    options: &RunOptions,
) -> Result<String, String> {
    if options.pf_check_jobs == 0 || options.pf_check_jobs > 16 {
        return Err("PF check job count must be between 1 and 16".to_string());
    }

    let workspace_wsl = windows_path_to_wsl(workspace_root)?;
    let disk_wsl = windows_path_to_wsl(disk_image)?;
    let artifact_dir = artifact_root.join("pf-check");
    let artifact_dir_wsl = windows_path_to_wsl(&artifact_dir)?;
    let timestamp_stream = windows_path_to_wsl(&workspace_root.join("tools/timestamp_stream.py"))?;
    let summarize_script =
        windows_path_to_wsl(&workspace_root.join("tools/summarize_boot_timed_log.py"))?;
    let runtime_dir_wsl = format!(
        "/tmp/capabilityos-qemu-{}/pf-check-$$",
        runtime_slug(workspace_root)
    );

    let mut qemu_parts = vec![
        "qemu-system-x86_64".to_string(),
        "-machine q35".to_string(),
        "-smp 4".to_string(),
        "-m 2G".to_string(),
        "-no-reboot".to_string(),
        "-monitor none".to_string(),
        format!("-d {QEMU_DEBUG_FLAGS}"),
        "-D \"$RUN_QEMU_LOG\"".to_string(),
        format!(
            "-drive if=pflash,format=raw,readonly=on,file={}",
            bash_quote(OVMF_CODE_PATH)
        ),
        "-drive if=pflash,format=raw,file=\"$RUN_OVMF_VARS\"".to_string(),
        "-drive if=none,file=\"$RUN_DISK\",format=raw,id=bootdisk".to_string(),
        "-device virtio-blk-pci,drive=bootdisk".to_string(),
        "-serial stdio".to_string(),
    ];
    append_display_devices(&mut qemu_parts, options.display);
    append_console_device(&mut qemu_parts, options.console);
    append_network_device(&mut qemu_parts);
    if options.kvm {
        qemu_parts.insert(1, "-cpu host".to_string());
        qemu_parts.insert(1, "-enable-kvm".to_string());
    }
    let qemu_cmd = qemu_parts.join(" \\\n    ");

    let mut script = String::new();
    script.push_str("set -euo pipefail\n");
    script.push_str(&format!("cd {}\n", bash_quote(&workspace_wsl)));
    script.push_str(&format!("DISK_IMG={}\n", bash_quote(&disk_wsl)));
    script.push_str(&format!("ARTIFACT_DIR={}\n", bash_quote(&artifact_dir_wsl)));
    script.push_str(&format!("RUNTIME_DIR={runtime_dir_wsl}\n"));
    script.push_str(&format!("JOBS={}\n", options.pf_check_jobs));
    script.push_str("mkdir -p \"$ARTIFACT_DIR\" \"$RUNTIME_DIR\"\n");
    script.push_str("rm -f \"$ARTIFACT_DIR/summary.txt\"\n");
    script.push_str(
        "if ! command -v python3 >/dev/null 2>&1; then echo 'missing python3'; exit 1; fi\n",
    );
    script.push_str("run_one() {\n");
    script.push_str("  id=\"$1\"\n");
    script.push_str("  RUN_DIR=\"$RUNTIME_DIR/run-$id\"\n");
    script.push_str("  OUT_DIR=\"$ARTIFACT_DIR/run-$id\"\n");
    script.push_str("  mkdir -p \"$RUN_DIR\" \"$OUT_DIR\"\n");
    script.push_str("  rm -f \"$OUT_DIR/qemu.log\" \"$OUT_DIR/serial-timed.log\" \"$OUT_DIR/boot-timing-summary.txt\" \"$OUT_DIR/pf-grep.txt\"\n");
    script.push_str("  RUN_DISK=\"$RUN_DIR/disk.img\"\n");
    script.push_str("  RUN_OVMF_VARS=\"$RUN_DIR/OVMF_VARS.fd\"\n");
    script.push_str("  RUN_QEMU_LOG=\"$RUN_DIR/qemu.log\"\n");
    script.push_str("  RUN_SERIAL_LOG=\"$RUN_DIR/serial-timed.log\"\n");
    script.push_str("  RUN_SUMMARY_LOG=\"$RUN_DIR/boot-timing-summary.txt\"\n");
    script.push_str("  cp \"$DISK_IMG\" \"$RUN_DISK\"\n");
    script.push_str(&format!(
        "  cp {} \"$RUN_OVMF_VARS\"\n",
        bash_quote(OVMF_VARS_TEMPLATE_PATH)
    ));
    script.push_str("  set +e\n");
    script.push_str(&format!(
        "  timeout --kill-after=2s {PF_CHECK_RUN_SECONDS}s \\\n    "
    ));
    script.push_str(&qemu_cmd);
    script.push_str(&format!(
        " | python3 {} | tee \"$RUN_SERIAL_LOG\"\n",
        bash_quote(&timestamp_stream)
    ));
    script.push_str("  qemu_status=${PIPESTATUS[0]}\n");
    script.push_str("  python3 ");
    script.push_str(&bash_quote(&summarize_script));
    script.push_str(" --check \"$RUN_SERIAL_LOG\" > \"$RUN_SUMMARY_LOG\" 2>&1\n");
    script.push_str("  summary_status=$?\n");
    script.push_str("  grep -E 'PAGE FAULT|#PF|PF_CAP|ACTION=terminate process|GENERAL PROTECTION|INVALID OPCODE|STACK SEGMENT FAULT|SEGMENT NOT PRESENT' \"$RUN_SERIAL_LOG\" > \"$RUN_DIR/pf-grep.txt\" 2>/dev/null\n");
    script.push_str("  pf_status=$?\n");
    script.push_str(
        "  boot_count=$(grep -c 'RAW ENTER MAIN' \"$RUN_SERIAL_LOG\" 2>/dev/null || true)\n",
    );
    script.push_str("  set -e\n");
    script.push_str("  cp \"$RUN_QEMU_LOG\" \"$OUT_DIR/qemu.log\" 2>/dev/null || true\n");
    script.push_str("  cp \"$RUN_SERIAL_LOG\" \"$OUT_DIR/serial-timed.log\" 2>/dev/null || true\n");
    script.push_str(
        "  cp \"$RUN_SUMMARY_LOG\" \"$OUT_DIR/boot-timing-summary.txt\" 2>/dev/null || true\n",
    );
    script.push_str("  cp \"$RUN_DIR/pf-grep.txt\" \"$OUT_DIR/pf-grep.txt\" 2>/dev/null || true\n");
    script.push_str(
        "  if [ \"$pf_status\" -eq 0 ]; then echo \"run-$id: PF marker found\"; return 20; fi\n",
    );
    script.push_str("  if [ \"$boot_count\" -gt 1 ]; then echo \"run-$id: reset loop after boot start ($boot_count boots)\"; return 21; fi\n");
    script.push_str("  if [ \"$qemu_status\" -ne 0 ] && [ \"$qemu_status\" -ne 124 ] && [ \"$qemu_status\" -ne 137 ]; then echo \"run-$id: qemu failed $qemu_status\"; return \"$qemu_status\"; fi\n");
    script.push_str("  if [ \"$qemu_status\" -eq 0 ] && [ \"$summary_status\" -ne 0 ]; then echo \"run-$id: qemu exited before smoke completed\"; return 22; fi\n");
    script.push_str("  if [ \"$summary_status\" -ne 0 ]; then echo \"run-$id: no PF markers; smoke summary incomplete\"; return 23; fi\n");
    script.push_str("  echo \"run-$id: no PF markers; smoke summary complete\"\n");
    script.push_str("  return 0\n");
    script.push_str("}\n");
    script.push_str("pids=\"\"\n");
    script.push_str("for id in $(seq 1 \"$JOBS\"); do\n");
    script.push_str("  (run_one \"$id\") > \"$RUNTIME_DIR/run-$id.stdout\" 2>&1 &\n");
    script.push_str("  pids=\"$pids $!:$id\"\n");
    script.push_str("done\n");
    script.push_str("status=0\n");
    script.push_str(": > \"$ARTIFACT_DIR/summary.txt\"\n");
    script.push_str("for pair in $pids; do\n");
    script.push_str("  pid=\"${pair%%:*}\"\n");
    script.push_str("  id=\"${pair##*:}\"\n");
    script.push_str("  if wait \"$pid\"; then run_status=0; else run_status=$?; fi\n");
    script.push_str("  cat \"$RUNTIME_DIR/run-$id.stdout\"\n");
    script.push_str("  cat \"$RUNTIME_DIR/run-$id.stdout\" >> \"$ARTIFACT_DIR/summary.txt\"\n");
    script.push_str("  if [ \"$run_status\" -ne 0 ]; then status=\"$run_status\"; fi\n");
    script.push_str("done\n");
    script.push_str("echo \"logs: $ARTIFACT_DIR\"\n");
    script.push_str("if [ \"$status\" -eq 0 ]; then echo \"parallel PF check ok: $JOBS runs\" | tee -a \"$ARTIFACT_DIR/summary.txt\"; exit 0; fi\n");
    script.push_str("exit \"$status\"\n");

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
