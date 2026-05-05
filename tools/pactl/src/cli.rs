use crate::build::{build_userland, BuildOptions};
use crate::config::{discover_apps, find_workspace_root, load_workspace_config, AppConfig};
use crate::disk::{ensure_disk_image, DiskEnsureMode};
use crate::manifest::generate_manifests;
use crate::run::{run_qemu, ConsoleBackend, DisplayBackend, RunOptions};
use crate::setup::{setup_workspace, SetupMode};
use crate::sync::{sync_bootfs, sync_rootfs};
use std::env;
use std::path::Path;

pub fn run(args: Vec<String>) -> Result<(), String> {
    if matches!(
        args.first().map(String::as_str),
        Some("-h" | "--help" | "help")
    ) {
        print_help();
        return Ok(());
    }

    let cwd =
        env::current_dir().map_err(|err| format!("failed to get current directory: {err}"))?;
    let workspace_root = find_workspace_root(&cwd)
        .ok_or_else(|| format!("could not find pactl.conf from {}", cwd.display()))?;
    let workspace = load_workspace_config(&workspace_root)?;

    match args.as_slice() {
        [] => cmd_plan(&workspace_root, &workspace),
        [cmd] if cmd == "plan" => cmd_plan(&workspace_root, &workspace),
        [section, sub] if section == "config" && sub == "path" => {
            println!("{}", workspace_root.join("pactl.conf").display());
            Ok(())
        }
        [section, sub] if section == "app" && sub == "list" => {
            cmd_app_list(&workspace_root, &workspace)
        }
        [section, sub, app_id] if section == "app" && sub == "show" => {
            cmd_app_show(&workspace_root, &workspace, app_id)
        }
        [section, sub] if section == "build" && sub == "userland" => {
            cmd_build_userland(&workspace_root, &workspace, None, BuildOptions::default())
        }
        [section, sub, app_id] if section == "build" && sub == "userland" => cmd_build_userland(
            &workspace_root,
            &workspace,
            Some(app_id),
            BuildOptions::default(),
        ),
        [section, sub, flag] if section == "build" && sub == "userland" && flag == "--fresh" => {
            cmd_build_userland(&workspace_root, &workspace, None, BuildOptions::fresh())
        }
        [section, sub, app_id, flag]
            if section == "build" && sub == "userland" && flag == "--fresh" =>
        {
            cmd_build_userland(
                &workspace_root,
                &workspace,
                Some(app_id),
                BuildOptions::fresh(),
            )
        }
        [section, sub] if section == "sync" && sub == "rootfs" => {
            cmd_sync_rootfs(&workspace_root, &workspace)
        }
        [section, sub] if section == "sync" && sub == "bootfs" => {
            cmd_sync_bootfs(&workspace_root, &workspace)
        }
        [section, sub] if section == "disk" && sub == "ensure" => {
            cmd_disk_ensure(&workspace_root, &workspace, DiskEnsureMode::UseConfig)
        }
        [section, sub, arg] if section == "disk" && sub == "ensure" && arg == "--fresh" => {
            cmd_disk_ensure(&workspace_root, &workspace, DiskEnsureMode::Always)
        }
        [cmd] if cmd == "setup" => cmd_setup(&workspace_root, &workspace, SetupMode::Diff),
        [section, mode] if section == "setup" && mode == "diff" => {
            cmd_setup(&workspace_root, &workspace, SetupMode::Diff)
        }
        [section, mode] if section == "setup" && mode == "full" => {
            cmd_setup(&workspace_root, &workspace, SetupMode::Full)
        }
        [section, flag] if section == "setup" && flag == "--diff" => {
            cmd_setup(&workspace_root, &workspace, SetupMode::Diff)
        }
        [section, flag] if section == "setup" && flag == "--full" => {
            cmd_setup(&workspace_root, &workspace, SetupMode::Full)
        }
        [cmd, rest @ ..] if cmd == "run" => cmd_run(&workspace_root, &workspace, rest),
        [section, sub] if section == "gen" && sub == "manifests" => {
            cmd_gen_manifests(&workspace_root, &workspace)
        }
        _ => {
            print_help();
            Err(format!("unknown command: {}", args.join(" ")))
        }
    }
}

fn print_help() {
    println!("pactl");
    println!();
    println!("commands:");
    println!("  pactl plan");
    println!("  pactl config path");
    println!("  pactl app list");
    println!("  pactl app show <id>");
    println!("  pactl build userland [id] [--fresh]");
    println!("  pactl disk ensure [--fresh]");
    println!("  pactl setup [diff|full]   (default: diff)");
    println!("  pactl sync rootfs");
    println!("  pactl sync bootfs");
    println!("  pactl run [--timed] [--pf-check <jobs>] [--no-kvm] [--dry-run] [--console=off|pty|stdio] [--display=gtk|none] [--split-windows]");
    println!("    run defaults: --console=stdio --display=gtk; timed/pf-check and --split-windows default to --console=pty");
    println!("  pactl gen manifests");
}

fn cmd_plan(
    workspace_root: &Path,
    workspace: &crate::config::WorkspaceConfig,
) -> Result<(), String> {
    let apps = discover_apps(workspace_root, workspace)?;
    println!("workspace: {}", workspace.workspace.name);
    println!("root: {}", workspace_root.display());
    println!(
        "kernel: {} (step: {})",
        workspace.kernel.dir, workspace.kernel.default_step
    );
    println!(
        "disk image: {} ({} MiB)",
        workspace.disk.image,
        workspace.disk.size_mib.unwrap_or(512)
    );
    if !workspace.userland.skip_kinds.is_empty() {
        println!("skip kinds: {}", workspace.userland.skip_kinds.join(", "));
    }
    if !workspace.userland.skip_apps.is_empty() {
        println!("skip apps: {}", workspace.userland.skip_apps.join(", "));
    }
    println!("apps dir: {}", workspace.userland.apps_dir);
    println!("apps: {}", apps.len());
    for app in apps {
        let publishes = if app.publish.is_empty() {
            "none".to_string()
        } else {
            app.publish
                .iter()
                .map(|entry| format!("{}:{}", entry.fs, entry.path))
                .collect::<Vec<_>>()
                .join(", ")
        };
        if let Some(startup) = &app.startup {
            println!(
                "  {} [{} / {}] -> {} | startup:{}",
                app.app.id, app.app.kind, app.app.role, publishes, startup.action
            );
        } else {
            println!(
                "  {} [{} / {}] -> {}",
                app.app.id, app.app.kind, app.app.role, publishes
            );
        }
    }
    Ok(())
}

fn cmd_app_list(
    workspace_root: &Path,
    workspace: &crate::config::WorkspaceConfig,
) -> Result<(), String> {
    let apps = discover_apps(workspace_root, workspace)?;
    for app in apps {
        println!(
            "{}\t{}\t{}\t{}",
            app.app.id,
            app.app.kind,
            app.app.role,
            app.config_path.display()
        );
    }
    Ok(())
}

fn cmd_app_show(
    workspace_root: &Path,
    workspace: &crate::config::WorkspaceConfig,
    app_id: &str,
) -> Result<(), String> {
    let apps = discover_apps(workspace_root, workspace)?;
    let app = apps
        .into_iter()
        .find(|app| app.app.id == app_id)
        .ok_or_else(|| format!("app not found: {app_id}"))?;
    print_app(&app);
    Ok(())
}

fn cmd_gen_manifests(
    workspace_root: &Path,
    workspace: &crate::config::WorkspaceConfig,
) -> Result<(), String> {
    let outputs = generate_manifests(workspace_root, workspace)?;
    println!("bootfs: {}", outputs.bootfs.display());
    println!("rootfs: {}", outputs.rootfs.display());
    println!("startup: {}", outputs.startup.display());
    Ok(())
}

fn print_app(app: &AppConfig) {
    println!("id: {}", app.app.id);
    println!("kind: {}", app.app.kind);
    println!("role: {}", app.app.role);
    println!("config: {}", app.config_path.display());
    match &app.source {
        crate::config::SourceConfig::Zig(src) => {
            println!("source.zig.entry: {}", src.entry);
            if !src.module.is_empty() {
                println!("source.zig.module: {}", src.module);
            }
            if !src.imports.is_empty() {
                println!("source.zig.imports: {}", src.imports.join(", "));
            }
            if !src.include_dirs.is_empty() {
                println!("source.zig.include_dirs: {}", src.include_dirs.join(", "));
            }
            if !src.c_sources.is_empty() {
                println!("source.zig.c_sources: {}", src.c_sources.join(", "));
            }
        }
        crate::config::SourceConfig::Cargo(src) => {
            println!("source.cargo.manifest: {}", src.manifest);
            println!("source.cargo.package: {}", src.package);
            if !src.bin.is_empty() {
                println!("source.cargo.bin: {}", src.bin);
            }
        }
        crate::config::SourceConfig::Capc(src) => {
            println!("source.capc.root: {}", src.root);
            println!("source.capc.entry: {}", src.entry);
        }
        crate::config::SourceConfig::File(src) => {
            println!("source.file.path: {}", src.path);
        }
        crate::config::SourceConfig::None => {}
    }
    println!("build.output_name: {}", app.build.output_name);
    println!("build.target: {}", app.build.target);
    println!("build.optimize: {}", app.build.optimize);
    if app.publish.is_empty() {
        println!("publish: none");
    } else {
        for entry in &app.publish {
            println!("publish.{}: {} {}", entry.id, entry.fs, entry.path);
        }
    }
    if let Some(startup) = &app.startup {
        println!("startup.action: {}", startup.action);
        println!("startup.name: {}", startup.name);
        println!("startup.label: {}", startup.label);
        println!("startup.load: {}", startup.load);
    }
}

fn cmd_build_userland(
    workspace_root: &Path,
    workspace: &crate::config::WorkspaceConfig,
    app_id: Option<&str>,
    options: BuildOptions,
) -> Result<(), String> {
    let built = build_userland(workspace_root, workspace, app_id, options)?;
    for artifact in built {
        println!("{}\t{}", artifact.app_id, artifact.output_path.display());
    }
    Ok(())
}

fn cmd_sync_rootfs(
    workspace_root: &Path,
    workspace: &crate::config::WorkspaceConfig,
) -> Result<(), String> {
    let outputs = sync_rootfs(workspace_root, workspace)?;
    println!("disk: {}", outputs.disk_image.display());
    println!("rootfs: {}", outputs.manifest.display());
    Ok(())
}

fn cmd_disk_ensure(
    workspace_root: &Path,
    workspace: &crate::config::WorkspaceConfig,
    mode: DiskEnsureMode,
) -> Result<(), String> {
    let outputs = ensure_disk_image(workspace_root, workspace, mode)?;
    println!("disk: {}", outputs.disk_image.display());
    println!("size: {} bytes", outputs.size_bytes);
    println!(
        "recreated: {}",
        if outputs.recreated { "yes" } else { "no" }
    );
    Ok(())
}

fn cmd_setup(
    workspace_root: &Path,
    workspace: &crate::config::WorkspaceConfig,
    mode: SetupMode,
) -> Result<(), String> {
    let outputs = setup_workspace(workspace_root, workspace, mode)?;
    println!(
        "mode: {}",
        match outputs.mode {
            SetupMode::Diff => "diff",
            SetupMode::Full => "full",
        }
    );
    println!(
        "kernel: {} ({})",
        outputs.kernel.build_dir.display(),
        outputs.kernel.step
    );
    println!("disk: {}", outputs.disk.disk_image.display());
    println!(
        "disk recreated: {}",
        if outputs.disk.recreated { "yes" } else { "no" }
    );
    println!("bootfs image: {}", outputs.bootfs.bootfs_image.display());
    println!("esp manifest: {}", outputs.bootfs.esp_manifest.display());
    println!("rootfs manifest: {}", outputs.rootfs.manifest.display());
    Ok(())
}

fn cmd_sync_bootfs(
    workspace_root: &Path,
    workspace: &crate::config::WorkspaceConfig,
) -> Result<(), String> {
    let outputs = sync_bootfs(workspace_root, workspace)?;
    println!("disk: {}", outputs.disk_image.display());
    println!("bootfs manifest: {}", outputs.bootfs_manifest.display());
    println!("bootfs image: {}", outputs.bootfs_image.display());
    println!("esp manifest: {}", outputs.esp_manifest.display());
    Ok(())
}

fn cmd_run(
    workspace_root: &Path,
    workspace: &crate::config::WorkspaceConfig,
    args: &[String],
) -> Result<(), String> {
    let mut options = RunOptions {
        timed: false,
        kvm: true,
        dry_run: false,
        pf_check_jobs: 0,
        console: ConsoleBackend::Pty,
        display: DisplayBackend::Gtk,
        split_windows: false,
    };

    let mut console_specified = false;
    let mut i = 0;
    while i < args.len() {
        let arg = &args[i];
        match arg.as_str() {
            "--timed" => options.timed = true,
            "--no-kvm" => options.kvm = false,
            "--dry-run" => options.dry_run = true,
            "--split-windows" => options.split_windows = true,
            "--console=off" => {
                options.console = ConsoleBackend::Off;
                console_specified = true;
            }
            "--console=pty" => {
                options.console = ConsoleBackend::Pty;
                console_specified = true;
            }
            "--console=stdio" => {
                options.console = ConsoleBackend::Stdio;
                console_specified = true;
            }
            "--display=gtk" => options.display = DisplayBackend::Gtk,
            "--display=none" => options.display = DisplayBackend::None,
            "--pf-check" => {
                i += 1;
                let jobs = args
                    .get(i)
                    .ok_or_else(|| "--pf-check requires a job count".to_string())?;
                options.pf_check_jobs = parse_pf_check_jobs(jobs)?;
                options.timed = true;
            }
            _ => {
                if let Some(jobs) = arg.strip_prefix("--pf-check=") {
                    options.pf_check_jobs = parse_pf_check_jobs(jobs)?;
                    options.timed = true;
                } else {
                    return Err(format!("unknown run option: {arg}"));
                }
            }
        }
        i += 1;
    }
    if !console_specified && !options.timed && options.pf_check_jobs == 0 {
        options.console = if options.split_windows {
            ConsoleBackend::Pty
        } else {
            ConsoleBackend::Stdio
        };
    }
    let plan = run_qemu(workspace_root, workspace, &options)?;
    println!("disk: {}", plan.disk_image.display());
    println!("ovmf vars: {}", plan.ovmf_vars.display());
    println!("qemu log: {}", plan.qemu_log.display());
    println!("kvm: {}", if options.kvm { "on" } else { "off" });
    println!("timed: {}", if options.timed { "on" } else { "off" });
    println!(
        "console: {}",
        match options.console {
            ConsoleBackend::Off => "off",
            ConsoleBackend::Pty => "pty",
            ConsoleBackend::Stdio => "stdio",
        }
    );
    println!(
        "display: {}",
        match options.display {
            DisplayBackend::Gtk => "gtk",
            DisplayBackend::None => "none",
        }
    );
    if options.pf_check_jobs != 0 {
        println!("pf-check jobs: {}", options.pf_check_jobs);
    }
    println!(
        "split windows: {}",
        if options.split_windows { "on" } else { "off" }
    );
    if let Some(serial_log) = plan.serial_log {
        println!("serial log: {}", serial_log.display());
    }
    if let Some(summary_log) = plan.summary_log {
        println!("summary: {}", summary_log.display());
    }
    if options.dry_run {
        println!("script:");
        println!("{}", plan.script);
    }
    Ok(())
}

fn parse_pf_check_jobs(value: &str) -> Result<usize, String> {
    let jobs = value
        .parse::<usize>()
        .map_err(|_| format!("invalid --pf-check job count: {value}"))?;
    if jobs == 0 || jobs > 16 {
        return Err("--pf-check job count must be between 1 and 16".to_string());
    }
    Ok(jobs)
}
