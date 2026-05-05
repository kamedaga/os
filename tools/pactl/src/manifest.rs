use crate::build::planned_artifact_path;
use crate::config::{
    app_is_skipped, discover_apps, include_skipped_artifacts_in_manifests, AppConfig, PublishEntry,
    SourceConfig, WorkspaceConfig,
};
use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};

pub struct GeneratedManifestPaths {
    pub bootfs: PathBuf,
    pub rootfs: PathBuf,
    pub startup: PathBuf,
}

#[derive(Clone)]
struct ManifestEntry {
    image_path: String,
    source_path: Option<PathBuf>,
    is_dir: bool,
}

#[derive(Clone)]
struct StartupNode {
    app_id: String,
    action: String,
    name: String,
    path: String,
    label: String,
    load: String,
    after: Vec<String>,
    requires: Vec<String>,
    provides: Vec<String>,
    ensure: Vec<String>,
    block: Vec<String>,
    device: Vec<String>,
    input: Vec<String>,
}

pub fn generate_manifests(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
) -> Result<GeneratedManifestPaths, String> {
    let apps = discover_manifest_apps(workspace_root, workspace)?;

    let bootfs_path = workspace_root.join(&workspace.manifests.bootfs);
    let rootfs_path = workspace_root.join(&workspace.manifests.rootfs);
    let startup_path = workspace_root.join(&workspace.manifests.startup);

    create_parent_dir(&bootfs_path)?;
    create_parent_dir(&rootfs_path)?;
    create_parent_dir(&startup_path)?;

    let startup_contents = render_startup_manifest(&apps)?;
    write_if_changed(&startup_path, &startup_contents)?;

    let bootfs_contents =
        render_fs_manifest("bootfs", workspace_root, workspace, &apps, &startup_path)?;
    write_if_changed(&bootfs_path, &bootfs_contents)?;

    let rootfs_contents =
        render_fs_manifest("rootfs", workspace_root, workspace, &apps, &startup_path)?;
    write_if_changed(&rootfs_path, &rootfs_contents)?;

    Ok(GeneratedManifestPaths {
        bootfs: bootfs_path,
        rootfs: rootfs_path,
        startup: startup_path,
    })
}

fn discover_manifest_apps(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
) -> Result<Vec<AppConfig>, String> {
    let apps = discover_apps(workspace_root, workspace)?;
    let mut selected = Vec::with_capacity(apps.len());
    let mut omitted_skipped_count = 0usize;
    for app in apps {
        if app_is_skipped(workspace, &app) {
            if !include_skipped_artifacts_in_manifests(workspace) {
                omitted_skipped_count += 1;
                continue;
            }
            let artifact = planned_artifact_path(workspace_root, workspace, &app);
            if !artifact_is_usable(&artifact)? {
                eprintln!(
                    "pactl: omitting app '{}' from generated manifests because kind '{}' is skipped and cached artifact is missing",
                    app.app.id, app.app.kind
                );
                continue;
            }
        }
        selected.push(app);
    }
    if omitted_skipped_count > 0 {
        eprintln!("pactl: omitted {omitted_skipped_count} skipped app(s) from generated manifests");
    }
    Ok(selected)
}

fn render_fs_manifest(
    fs_name: &str,
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    apps: &[AppConfig],
    startup_path: &Path,
) -> Result<String, String> {
    let mut entries = Vec::new();

    for dir in &workspace.publish_dirs {
        if dir.fs == fs_name {
            entries.push(ManifestEntry {
                image_path: dir.path.clone(),
                source_path: None,
                is_dir: true,
            });
        }
    }

    for app in apps {
        let source_path = resolve_app_source_path(workspace_root, workspace, app)?;
        for publish in &app.publish {
            if publish.fs != fs_name {
                continue;
            }
            entries.push(ManifestEntry {
                image_path: publish.path.clone(),
                source_path: Some(source_path.clone()),
                is_dir: false,
            });
        }
    }

    if workspace
        .startup_manifest
        .include
        .iter()
        .any(|item| item == fs_name)
    {
        entries.push(ManifestEntry {
            image_path: workspace.startup_manifest.path.clone(),
            source_path: Some(startup_path.to_path_buf()),
            is_dir: false,
        });
    }

    let mut dirs = entries
        .iter()
        .filter(|entry| entry.is_dir)
        .cloned()
        .collect::<Vec<_>>();
    let mut files = entries
        .iter()
        .filter(|entry| !entry.is_dir)
        .cloned()
        .collect::<Vec<_>>();
    files.sort_by(|lhs, rhs| lhs.image_path.cmp(&rhs.image_path));
    dirs.append(&mut files);
    let entries = dirs;

    let mut seen_paths = BTreeSet::new();
    for entry in &entries {
        if !seen_paths.insert(entry.image_path.clone()) {
            return Err(format!(
                "duplicate {fs_name} publish path: {}",
                entry.image_path
            ));
        }
    }

    let mut out = String::from("# Generated by pactl. DO NOT EDIT.\n");
    for entry in entries {
        out.push_str(&entry.image_path);
        out.push('=');
        if entry.is_dir {
            out.push_str("@dir");
        } else if let Some(source_path) = entry.source_path {
            out.push_str(&source_path.display().to_string());
        } else {
            return Err(format!(
                "missing source path for {} entry {}",
                fs_name, entry.image_path
            ));
        }
        out.push('\n');
    }
    Ok(out)
}

fn render_startup_manifest(apps: &[AppConfig]) -> Result<String, String> {
    let ordered = order_startup_nodes(apps)?;
    let mut out = String::from("# Generated by pactl. DO NOT EDIT.\n");
    for node in ordered {
        out.push_str("action=");
        out.push_str(&node.action);
        out.push(' ');
        out.push_str("name=");
        out.push_str(&node.name);
        out.push(' ');
        out.push_str("path=");
        out.push_str(&node.path);
        out.push(' ');
        out.push_str("label=");
        out.push_str(&node.label);
        out.push(' ');
        out.push_str("load=");
        out.push_str(&node.load);

        if !node.block.is_empty() {
            if node.block.len() != 1 {
                return Err(format!(
                    "startup block selector must have exactly one value for app {}",
                    node.app_id
                ));
            }
            out.push(' ');
            out.push_str("block=");
            out.push_str(&node.block[0]);
        }
        if !node.input.is_empty() {
            if node.input.len() != 1 {
                return Err(format!(
                    "startup input selector must have exactly one value for app {}",
                    node.app_id
                ));
            }
            out.push(' ');
            out.push_str("input=");
            out.push_str(&node.input[0]);
        }
        if !node.device.is_empty() {
            if node.device.len() != 1 {
                return Err(format!(
                    "startup device selector must have exactly one value for app {}",
                    node.app_id
                ));
            }
            out.push(' ');
            out.push_str("device=");
            out.push_str(&node.device[0]);
        }
        append_list_field(&mut out, "after", &node.after);
        append_list_field(&mut out, "requires", &node.requires);
        append_list_field(&mut out, "ensure", &node.ensure);
        append_list_field(&mut out, "provides", &node.provides);
        out.push('\n');
    }
    Ok(out)
}

fn append_list_field(out: &mut String, key: &str, values: &[String]) {
    if values.is_empty() {
        return;
    }
    out.push(' ');
    out.push_str(key);
    out.push('=');
    out.push_str(&values.join(","));
}

fn order_startup_nodes(apps: &[AppConfig]) -> Result<Vec<StartupNode>, String> {
    let mut nodes = Vec::new();
    let mut names = BTreeSet::new();

    for app in apps {
        let Some(startup) = &app.startup else {
            continue;
        };
        let publish = find_publish(app, &startup.publish).ok_or_else(|| {
            format!(
                "startup.publish '{}' missing for app {}",
                startup.publish, app.app.id
            )
        })?;
        let node = StartupNode {
            app_id: app.app.id.clone(),
            action: startup.action.clone(),
            name: startup.name.clone(),
            path: publish.path.clone(),
            label: startup.label.clone(),
            load: startup.load.clone(),
            after: startup.after.clone(),
            requires: startup.requires.clone(),
            provides: startup.provides.clone(),
            ensure: startup.ensure.clone(),
            block: startup.block.clone(),
            device: startup.device.clone(),
            input: startup.input.clone(),
        };
        if node.action.is_empty()
            || node.name.is_empty()
            || node.path.is_empty()
            || node.load.is_empty()
        {
            return Err(format!("startup fields missing for app {}", app.app.id));
        }
        if !names.insert(node.name.clone()) {
            return Err(format!("duplicate startup name: {}", node.name));
        }
        nodes.push(node);
    }

    for node in &nodes {
        for dep in &node.after {
            if !names.contains(dep) {
                return Err(format!(
                    "startup.after for app {} references unknown startup name '{}'",
                    node.app_id, dep
                ));
            }
        }
    }

    nodes.sort_by(|lhs, rhs| lhs.name.cmp(&rhs.name));

    let mut emitted = BTreeSet::new();
    let mut ordered = Vec::with_capacity(nodes.len());
    while !nodes.is_empty() {
        let ready_index = nodes
            .iter()
            .position(|node| node.after.iter().all(|dep| emitted.contains(dep)));
        let Some(index) = ready_index else {
            let cycle = nodes
                .iter()
                .map(|node| node.name.as_str())
                .collect::<Vec<_>>()
                .join(", ");
            return Err(format!("startup dependency cycle detected: {cycle}"));
        };
        let node = nodes.remove(index);
        emitted.insert(node.name.clone());
        ordered.push(node);
    }

    Ok(ordered)
}

fn find_publish<'a>(app: &'a AppConfig, publish_id: &str) -> Option<&'a PublishEntry> {
    app.publish.iter().find(|entry| entry.id == publish_id)
}

fn resolve_app_source_path(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
    app: &AppConfig,
) -> Result<PathBuf, String> {
    match &app.source {
        SourceConfig::File(_)
        | SourceConfig::Zig(_)
        | SourceConfig::Cargo(_)
        | SourceConfig::Capc(_) => Ok(planned_artifact_path(workspace_root, workspace, app)),
        SourceConfig::None => Err(format!("no source configured for app {}", app.app.id)),
    }
}

fn create_parent_dir(path: &Path) -> Result<(), String> {
    let Some(parent) = path.parent() else {
        return Ok(());
    };
    fs::create_dir_all(parent)
        .map_err(|err| format!("failed to create {}: {err}", parent.display()))
}

fn write_if_changed(path: &Path, contents: &str) -> Result<(), String> {
    if let Ok(existing) = fs::read_to_string(path) {
        if existing == contents {
            return Ok(());
        }
    }
    fs::write(path, contents).map_err(|err| format!("failed to write {}: {err}", path.display()))
}

fn artifact_is_usable(path: &Path) -> Result<bool, String> {
    match fs::metadata(path) {
        Ok(metadata) => Ok(metadata.is_file() && metadata.len() > 0),
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(err) => Err(format!("failed to inspect {}: {err}", path.display())),
    }
}
