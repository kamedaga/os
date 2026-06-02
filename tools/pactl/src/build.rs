use crate::config::{
    app_is_skipped, discover_apps, AppConfig, CargoSource, FileSource, SourceConfig,
    WorkspaceConfig, ZigSource,
};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::SystemTime;

pub struct BuildArtifact {
    pub app_id: String,
    pub output_path: PathBuf,
}

#[derive(Copy, Clone)]
pub struct BuildOptions {
    pub force: bool,
    pub warn_stale_cargo: bool,
    pub ignore_skip_kinds: bool,
}

impl BuildOptions {
    pub const fn default() -> Self {
        Self {
            force: false,
            warn_stale_cargo: true,
            ignore_skip_kinds: false,
        }
    }

    pub const fn fresh() -> Self {
        Self {
            force: true,
            warn_stale_cargo: false,
            ignore_skip_kinds: false,
        }
    }
}

pub fn build_userland(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    app_filter: Option<&str>,
    options: BuildOptions,
) -> Result<Vec<BuildArtifact>, String> {
    let apps = discover_apps(workspace_root, workspace)?;
    let selected = if let Some(app_id) = app_filter {
        let app = apps
            .into_iter()
            .find(|app| app.app.id == app_id)
            .ok_or_else(|| format!("app not found: {app_id}"))?;
        vec![app]
    } else {
        apps
    };
    let allow_skipped_kind_build = app_filter.is_some() || options.ignore_skip_kinds;

    let mut built = Vec::with_capacity(selected.len());
    for app in selected {
        let output_path = planned_artifact_path(workspace_root, workspace, &app);
        if let Some(parent) = output_path.parent() {
            fs::create_dir_all(parent)
                .map_err(|err| format!("failed to create {}: {err}", parent.display()))?;
        }

        if !allow_skipped_kind_build && app_is_skipped(workspace, &app) {
            if output_is_usable(&output_path)? {
                built.push(BuildArtifact {
                    app_id: app.app.id,
                    output_path,
                });
            } else {
                eprintln!(
                    "pactl: skipping app '{}' (kind: {}) because [userland].skip_kinds includes it and no cached artifact exists",
                    app.app.id, app.app.kind
                );
            }
            continue;
        }

        match &app.source {
            SourceConfig::Zig(src) => {
                build_zig_app(workspace_root, workspace, &app, src, &output_path)?
            }
            SourceConfig::Cargo(src) => {
                build_cargo_app(workspace_root, workspace, &app, src, &output_path, options)?
            }
            SourceConfig::File(src) => {
                build_file_app(workspace_root, workspace, &app, src, &output_path, options)?
            }
            SourceConfig::Capc(_) => {
                return Err(format!(
                    "capc build is not implemented yet for app {}",
                    app.app.id
                ));
            }
            SourceConfig::None => {
                return Err(format!("no source configured for app {}", app.app.id));
            }
        }

        built.push(BuildArtifact {
            app_id: app.app.id,
            output_path,
        });
    }

    Ok(built)
}

pub fn planned_artifact_path(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    app: &AppConfig,
) -> PathBuf {
    workspace_root
        .join(&workspace.artifacts.dir)
        .join("userland")
        .join(&app.app.id)
        .join(&app.build.output_name)
}

fn build_zig_app(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    app: &AppConfig,
    src: &ZigSource,
    output_path: &Path,
) -> Result<(), String> {
    let zig = tool_or_default(&workspace.toolchain.zig, "zig");
    let cache_root = workspace_root
        .join(&workspace.artifacts.dir)
        .join("zig-cache");
    let local_cache = cache_root.join("local");
    let global_cache = cache_root.join("global");
    fs::create_dir_all(&local_cache)
        .map_err(|err| format!("failed to create {}: {err}", local_cache.display()))?;
    fs::create_dir_all(&global_cache)
        .map_err(|err| format!("failed to create {}: {err}", global_cache.display()))?;

    let mut cmd = Command::new(zig);
    cmd.current_dir(workspace_root);
    cmd.arg("build-exe");
    cmd.arg("--name").arg(binary_stem(&app.build.output_name));
    cmd.arg("-target").arg(&app.build.target);
    cmd.arg("-O").arg(&app.build.optimize);
    cmd.arg("-mcmodel=small");
    cmd.arg("-mno-red-zone");

    for include_dir in &src.include_dirs {
        cmd.arg(format!("-I{}", normalize_slashes(include_dir)));
    }
    if !src.c_sources.is_empty() {
        cmd.arg("-cflags");
        for flag in &src.cflags {
            cmd.arg(flag);
        }
        cmd.arg("--");
        for c_source in &src.c_sources {
            cmd.arg(normalize_slashes(c_source));
        }
    }

    let imports = zig_imports(src);
    for import in &imports {
        match import.as_str() {
            "abi_root" | "persistent_fs_layout" => {
                cmd.arg("--dep").arg(import);
            }
            other => {
                return Err(format!(
                    "unsupported Zig import '{}' for app {}",
                    other, app.app.id
                ));
            }
        }
    }
    cmd.arg(format!("-Mroot={}", normalize_slashes(&src.entry)));

    if imports.iter().any(|item| item == "abi_root") {
        cmd.arg("-target").arg(&app.build.target);
        cmd.arg("-O").arg(&app.build.optimize);
        cmd.arg("-mcmodel=small");
        cmd.arg("-mno-red-zone");
        cmd.arg("--dep").arg("persistent_fs_layout");
        cmd.arg("-Mabi_root=userland/programs/abi/abi_root.zig");
    }
    if imports
        .iter()
        .any(|item| item == "abi_root" || item == "persistent_fs_layout")
    {
        cmd.arg("-Mpersistent_fs_layout=userland/programs/abi/persistent_fs_layout.zig");
    }

    cmd.arg("-fPIE");
    cmd.arg("-fentry=_start");
    cmd.arg("-z").arg("common-page-size=4096");
    cmd.arg("-z").arg("max-page-size=4096");
    if app.build.strip {
        cmd.arg("-fstrip");
    }
    cmd.arg(zig_workspace_path_flag(
        workspace_root,
        "-femit-bin=",
        output_path,
    ));
    cmd.arg("--cache-dir")
        .arg(zig_workspace_path(workspace_root, &local_cache));
    cmd.arg("--global-cache-dir")
        .arg(zig_workspace_path(workspace_root, &global_cache));

    run_command(&format!("zig build for app {}", app.app.id), &mut cmd)
}

fn build_cargo_app(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    app: &AppConfig,
    src: &CargoSource,
    output_path: &Path,
    options: BuildOptions,
) -> Result<(), String> {
    if output_is_usable(output_path)?
        && !options.force
        && options.warn_stale_cargo
        && cargo_inputs_newer_than_output(workspace_root, src, output_path)?
    {
        eprintln!(
            "pactl: cargo artifact for '{}' is stale; rebuilding {}",
            app.app.id,
            output_path.display()
        );
    }

    let cargo = tool_or_default(&workspace.toolchain.cargo, "cargo");
    let manifest_path = workspace_root.join(&src.manifest);
    let target_dir = if src.target_dir.is_empty() {
        workspace_root
            .join(&workspace.artifacts.dir)
            .join("cargo-target")
    } else {
        workspace_root.join(&src.target_dir)
    };
    let profile_dir = cargo_profile_dir(&app.build.optimize);

    let mut cmd = Command::new(cargo);
    cmd.current_dir(workspace_root);
    cmd.arg("build");
    cmd.arg("--manifest-path").arg(&manifest_path);
    cmd.arg("--package").arg(&src.package);
    if !src.bin.is_empty() {
        cmd.arg("--bin").arg(&src.bin);
    }
    if !src.target.is_empty() {
        cmd.arg("--target").arg(&src.target);
    }
    if profile_dir == "release" {
        cmd.arg("--release");
    }
    cmd.arg("--target-dir").arg(&target_dir);

    // Always defer freshness decisions to Cargo itself. A fast no-op cargo build
    // is cheaper and more correct than trying to approximate the dependency graph
    // here and accidentally publishing a stale ELF into rootfs.
    run_command(&format!("cargo build for app {}", app.app.id), &mut cmd)?;

    let executable_name = if src.bin.is_empty() {
        &src.package
    } else {
        &src.bin
    };
    let built_path = if src.target.is_empty() {
        target_dir.join(profile_dir).join(executable_name)
    } else {
        target_dir
            .join(&src.target)
            .join(profile_dir)
            .join(executable_name)
    };
    if !built_path.is_file() {
        return Err(format!(
            "cargo output for app {} not found: {}",
            app.app.id,
            built_path.display()
        ));
    }
    copy_if_changed(&built_path, output_path)
}

fn build_file_app(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    app: &AppConfig,
    src: &FileSource,
    output_path: &Path,
    _options: BuildOptions,
) -> Result<(), String> {
    let source_path = workspace_root.join(&src.path);
    let should_rebuild = !src.rebuild_tool.is_empty();

    if should_rebuild {
        let mut cmd = Command::new(resolve_tool(workspace, &src.rebuild_tool));
        let rebuild_dir = if src.rebuild_dir.is_empty() {
            workspace_root.to_path_buf()
        } else {
            workspace_root.join(&src.rebuild_dir)
        };
        cmd.current_dir(&rebuild_dir);
        for arg in &src.rebuild_args {
            cmd.arg(arg);
        }
        run_command(
            &format!("rebuild file source for app {}", app.app.id),
            &mut cmd,
        )?;
    }

    if source_path.is_dir() {
        copy_dir_if_changed(&source_path, output_path)?;
        return Ok(());
    }

    if !source_path.is_file() {
        return Err(format!(
            "prebuilt source for app {} does not exist: {}",
            app.app.id,
            source_path.display()
        ));
    }
    copy_if_changed(&source_path, output_path)
}

fn output_is_usable(path: &Path) -> Result<bool, String> {
    match fs::metadata(path) {
        Ok(metadata) => Ok((metadata.is_file() && metadata.len() > 0) || metadata.is_dir()),
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(err) => Err(format!("failed to inspect {}: {err}", path.display())),
    }
}

fn cargo_inputs_newer_than_output(
    workspace_root: &Path,
    src: &CargoSource,
    output_path: &Path,
) -> Result<bool, String> {
    let output_time = modified_time(output_path)?;
    let manifest_path = workspace_root.join(&src.manifest);
    let manifest_dir = manifest_path
        .parent()
        .ok_or_else(|| format!("invalid cargo manifest path: {}", manifest_path.display()))?;
    let manifest_dir = manifest_dir.to_path_buf();

    let mut candidates = vec![
        manifest_path,
        manifest_dir.join("Cargo.lock"),
        manifest_dir.join(&src.package),
    ];
    for shared in ["rt_core", "rt_alloc", "rt_handle", "rt_io", "cap_std"] {
        candidates.push(manifest_dir.join(shared));
    }

    for candidate in candidates {
        if path_has_newer_files(&candidate, output_time)? {
            return Ok(true);
        }
    }
    Ok(false)
}

fn path_has_newer_files(path: &Path, cutoff: SystemTime) -> Result<bool, String> {
    match fs::metadata(path) {
        Ok(metadata) if metadata.is_file() => Ok(modified_time(path)? > cutoff),
        Ok(metadata) if metadata.is_dir() => {
            for entry in fs::read_dir(path)
                .map_err(|err| format!("failed to read {}: {err}", path.display()))?
            {
                let entry = entry
                    .map_err(|err| format!("failed to read entry in {}: {err}", path.display()))?;
                if path_has_newer_files(&entry.path(), cutoff)? {
                    return Ok(true);
                }
            }
            Ok(false)
        }
        Ok(_) => Ok(false),
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(err) => Err(format!("failed to inspect {}: {err}", path.display())),
    }
}

fn modified_time(path: &Path) -> Result<SystemTime, String> {
    fs::metadata(path)
        .and_then(|meta| meta.modified())
        .map_err(|err| format!("failed to read modified time for {}: {err}", path.display()))
}

fn tool_or_default<'a>(value: &'a str, fallback: &'a str) -> &'a str {
    if value.is_empty() {
        fallback
    } else {
        value
    }
}

fn resolve_tool<'a>(workspace: &'a WorkspaceConfig, tool: &'a str) -> &'a str {
    match tool {
        "zig" => tool_or_default(&workspace.toolchain.zig, "zig"),
        "cargo" => tool_or_default(&workspace.toolchain.cargo, "cargo"),
        "pwsh" | "pwsh.exe" => tool_or_default(&workspace.toolchain.pwsh, "pwsh.exe"),
        _ => tool,
    }
}

fn binary_stem(output_name: &str) -> String {
    Path::new(output_name)
        .file_stem()
        .and_then(|value| value.to_str())
        .unwrap_or(output_name)
        .to_string()
}

fn zig_imports(src: &ZigSource) -> Vec<String> {
    let mut imports = src.imports.clone();
    if imports.is_empty() && !src.module.is_empty() {
        imports.push(src.module.clone());
    }
    imports
}

fn cargo_profile_dir(optimize: &str) -> &'static str {
    if optimize.eq_ignore_ascii_case("release")
        || optimize.eq_ignore_ascii_case("releasesmall")
        || optimize.eq_ignore_ascii_case("releasefast")
        || optimize.eq_ignore_ascii_case("releasesafe")
    {
        "release"
    } else {
        "debug"
    }
}

fn zig_path(path: &Path) -> String {
    path.display().to_string().replace('\\', "/")
}

fn zig_workspace_path(workspace_root: &Path, path: &Path) -> String {
    if let Ok(relative) = path.strip_prefix(workspace_root) {
        zig_path(relative)
    } else {
        zig_path(path)
    }
}

fn zig_workspace_path_flag(workspace_root: &Path, prefix: &str, path: &Path) -> String {
    format!("{prefix}{}", zig_workspace_path(workspace_root, path))
}

fn normalize_slashes(path: &str) -> String {
    path.replace('\\', "/")
}

fn run_command(label: &str, cmd: &mut Command) -> Result<(), String> {
    let command_debug = format!("{cmd:?}");
    eprintln!("pactl: running {label}: {command_debug}");
    let status = cmd
        .status()
        .map_err(|err| format!("failed to run {label}: {err}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!(
            "{label} failed with exit code {:?}: {command_debug}",
            status.code()
        ))
    }
}

fn copy_if_changed(source_path: &Path, output_path: &Path) -> Result<(), String> {
    let source = fs::read(source_path)
        .map_err(|err| format!("failed to read {}: {err}", source_path.display()))?;
    if let Ok(existing) = fs::read(output_path) {
        if existing == source {
            return Ok(());
        }
    }
    fs::write(output_path, source)
        .map_err(|err| format!("failed to write {}: {err}", output_path.display()))
}

fn copy_dir_if_changed(source_path: &Path, output_path: &Path) -> Result<(), String> {
    if output_path.exists() {
        if output_path.is_dir() {
            fs::remove_dir_all(output_path)
                .map_err(|err| format!("failed to remove {}: {err}", output_path.display()))?;
        } else {
            fs::remove_file(output_path)
                .map_err(|err| format!("failed to remove {}: {err}", output_path.display()))?;
        }
    }
    copy_dir_all(source_path, output_path)
}

fn copy_dir_all(source_path: &Path, output_path: &Path) -> Result<(), String> {
    fs::create_dir_all(output_path)
        .map_err(|err| format!("failed to create {}: {err}", output_path.display()))?;
    for entry in fs::read_dir(source_path)
        .map_err(|err| format!("failed to read {}: {err}", source_path.display()))?
    {
        let entry = entry
            .map_err(|err| format!("failed to read {} entry: {err}", source_path.display()))?;
        let source_child = entry.path();
        let output_child = output_path.join(entry.file_name());
        let file_type = entry
            .file_type()
            .map_err(|err| format!("failed to inspect {}: {err}", source_child.display()))?;
        if file_type.is_dir() {
            copy_dir_all(&source_child, &output_child)?;
        } else if file_type.is_file() {
            fs::copy(&source_child, &output_child).map_err(|err| {
                format!(
                    "failed to copy {} to {}: {err}",
                    source_child.display(),
                    output_child.display()
                )
            })?;
        }
    }
    Ok(())
}
