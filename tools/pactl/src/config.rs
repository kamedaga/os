use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Default)]
pub struct WorkspaceConfig {
    pub schema_version: u32,
    pub workspace: WorkspaceSection,
    pub artifacts: ArtifactsSection,
    pub kernel: KernelSection,
    pub userland: UserlandSection,
    pub disk: DiskSection,
    pub manifests: ManifestsSection,
    pub publish_dirs: Vec<PublishDir>,
    pub startup_manifest: StartupManifestSection,
    pub toolchain: ToolchainSection,
    pub run: RunSection,
}

#[derive(Debug, Clone, Default)]
pub struct WorkspaceSection {
    pub name: String,
    pub root: String,
}

#[derive(Debug, Clone, Default)]
pub struct ArtifactsSection {
    pub dir: String,
    pub state_dir: String,
}

#[derive(Debug, Clone, Default)]
pub struct KernelSection {
    pub dir: String,
    pub default_step: String,
}

#[derive(Debug, Clone, Default)]
pub struct UserlandSection {
    pub apps_dir: String,
    pub skip_kinds: Vec<String>,
    pub skip_apps: Vec<String>,
    pub include_skipped_artifacts_in_manifests: Option<bool>,
}

#[derive(Debug, Clone, Default)]
pub struct DiskSection {
    pub image: String,
    pub size_mib: Option<u32>,
    pub recreate: String,
    pub partitions: Vec<DiskPartition>,
}

#[derive(Debug, Clone, Default)]
pub struct DiskPartition {
    pub id: String,
    pub index: u32,
    pub format: String,
    pub size_mib: Option<u32>,
    pub grow: bool,
}

#[derive(Debug, Clone, Default)]
pub struct ManifestsSection {
    pub dir: String,
    pub bootfs: String,
    pub rootfs: String,
    pub startup: String,
}

#[derive(Debug, Clone, Default)]
pub struct PublishDir {
    pub fs: String,
    pub path: String,
}

#[derive(Debug, Clone, Default)]
pub struct StartupManifestSection {
    pub path: String,
    pub include: Vec<String>,
}

#[derive(Debug, Clone, Default)]
pub struct ToolchainSection {
    pub zig: String,
    pub cargo: String,
    pub pwsh: String,
}

#[derive(Debug, Clone, Default)]
pub struct RunSection {
    pub default: String,
    pub profiles: BTreeMap<String, RunProfile>,
}

#[derive(Debug, Clone, Default)]
pub struct RunProfile {
    pub kind: String,
    pub path: String,
}

#[derive(Debug, Clone, Default)]
pub struct AppConfig {
    pub config_path: PathBuf,
    pub schema_version: u32,
    pub app: AppMeta,
    pub source: SourceConfig,
    pub build: BuildConfig,
    pub publish: Vec<PublishEntry>,
    pub startup: Option<StartupConfig>,
}

#[derive(Debug, Clone, Default)]
pub struct AppMeta {
    pub id: String,
    pub kind: String,
    pub role: String,
}

#[derive(Debug, Clone, Default)]
pub struct BuildConfig {
    pub format: String,
    pub target: String,
    pub optimize: String,
    pub output_name: String,
    pub strip: bool,
}

#[derive(Debug, Clone)]
pub enum SourceConfig {
    None,
    Zig(ZigSource),
    Cargo(CargoSource),
    Capc(CapcSource),
    File(FileSource),
}

impl Default for SourceConfig {
    fn default() -> Self {
        Self::None
    }
}

#[derive(Debug, Clone, Default)]
pub struct ZigSource {
    pub entry: String,
    pub module: String,
    pub imports: Vec<String>,
    pub c_sources: Vec<String>,
    pub include_dirs: Vec<String>,
    pub cflags: Vec<String>,
}

#[derive(Debug, Clone, Default)]
pub struct CargoSource {
    pub manifest: String,
    pub package: String,
    pub bin: String,
    pub target: String,
    pub target_dir: String,
}

#[derive(Debug, Clone, Default)]
pub struct CapcSource {
    pub root: String,
    pub entry: String,
    pub crt: String,
    pub sources: Vec<String>,
    pub include_dirs: Vec<String>,
}

#[derive(Debug, Clone, Default)]
pub struct FileSource {
    pub path: String,
    pub rebuild_tool: String,
    pub rebuild_dir: String,
    pub rebuild_args: Vec<String>,
}

#[derive(Debug, Clone, Default)]
pub struct PublishEntry {
    pub id: String,
    pub fs: String,
    pub path: String,
}

#[derive(Debug, Clone, Default)]
pub struct StartupConfig {
    pub publish: String,
    pub action: String,
    pub name: String,
    pub label: String,
    pub load: String,
    pub after: Vec<String>,
    pub requires: Vec<String>,
    pub provides: Vec<String>,
    pub ensure: Vec<String>,
    pub block: Vec<String>,
    pub device: Vec<String>,
    pub input: Vec<String>,
}

#[derive(Debug, Clone)]
struct SectionPath {
    parts: Vec<String>,
    is_array: bool,
}

#[derive(Debug, Clone)]
enum Value {
    String(String),
    Bool(bool),
    Integer(i64),
    StringArray(Vec<String>),
}

impl Value {
    fn into_string(self, ctx: &str) -> Result<String, String> {
        match self {
            Self::String(value) => Ok(value),
            _ => Err(format!("{ctx}: expected string")),
        }
    }

    fn into_bool(self, ctx: &str) -> Result<bool, String> {
        match self {
            Self::Bool(value) => Ok(value),
            _ => Err(format!("{ctx}: expected bool")),
        }
    }

    fn into_u32(self, ctx: &str) -> Result<u32, String> {
        match self {
            Self::Integer(value) if value >= 0 => Ok(value as u32),
            _ => Err(format!("{ctx}: expected non-negative integer")),
        }
    }

    fn into_string_array(self, ctx: &str) -> Result<Vec<String>, String> {
        match self {
            Self::StringArray(values) => Ok(values),
            Self::String(value) => Ok(vec![value]),
            _ => Err(format!("{ctx}: expected string array")),
        }
    }
}

pub fn find_workspace_root(start: &Path) -> Option<PathBuf> {
    let mut current = Some(start);
    while let Some(path) = current {
        if path.join("pactl.conf").is_file() {
            return Some(path.to_path_buf());
        }
        current = path.parent();
    }
    None
}

pub fn load_workspace_config(workspace_root: &Path) -> Result<WorkspaceConfig, String> {
    let config_path = workspace_root.join("pactl.conf");
    let contents = fs::read_to_string(&config_path)
        .map_err(|err| format!("failed to read {}: {err}", config_path.display()))?;
    let mut config = WorkspaceConfig::default();
    parse_document(&contents, |section, key, value| {
        assign_workspace_value(&mut config, section, key, value)
    })?;
    validate_workspace_config(&config, &config_path)?;
    Ok(config)
}

pub fn discover_apps(
    workspace_root: &Path,
    workspace: &WorkspaceConfig,
) -> Result<Vec<AppConfig>, String> {
    let apps_root = workspace_root.join(&workspace.userland.apps_dir);
    let mut config_paths = Vec::new();
    collect_named_files(&apps_root, "app.conf", &mut config_paths)?;
    config_paths.sort();

    let mut apps = Vec::with_capacity(config_paths.len());
    let mut seen_ids = BTreeSet::new();
    for config_path in config_paths {
        let mut app = load_app_config(&config_path)?;
        if !seen_ids.insert(app.app.id.clone()) {
            return Err(format!("duplicate app id: {}", app.app.id));
        }
        app.config_path = config_path;
        apps.push(app);
    }
    apps.sort_by(|lhs, rhs| lhs.app.id.cmp(&rhs.app.id));
    Ok(apps)
}

fn collect_named_files(root: &Path, file_name: &str, out: &mut Vec<PathBuf>) -> Result<(), String> {
    if !root.exists() {
        return Ok(());
    }
    for entry in
        fs::read_dir(root).map_err(|err| format!("failed to read {}: {err}", root.display()))?
    {
        let entry = entry.map_err(|err| format!("failed to read directory entry: {err}"))?;
        let path = entry.path();
        let file_type = entry
            .file_type()
            .map_err(|err| format!("failed to inspect {}: {err}", path.display()))?;
        if file_type.is_dir() {
            collect_named_files(&path, file_name, out)?;
        } else if file_type.is_file()
            && path.file_name().and_then(|name| name.to_str()) == Some(file_name)
        {
            out.push(path);
        }
    }
    Ok(())
}

fn load_app_config(config_path: &Path) -> Result<AppConfig, String> {
    let contents = fs::read_to_string(config_path)
        .map_err(|err| format!("failed to read {}: {err}", config_path.display()))?;
    let mut config = AppConfig {
        config_path: config_path.to_path_buf(),
        ..AppConfig::default()
    };
    parse_document(&contents, |section, key, value| {
        assign_app_value(&mut config, section, key, value)
    })?;
    validate_app_config(&config)?;
    Ok(config)
}

fn validate_workspace_config(config: &WorkspaceConfig, config_path: &Path) -> Result<(), String> {
    if config.schema_version != 1 {
        return Err(format!(
            "{}: unsupported schema_version {}",
            config_path.display(),
            config.schema_version
        ));
    }
    if config.workspace.name.is_empty() {
        return Err(format!(
            "{}: missing [workspace].name",
            config_path.display()
        ));
    }
    if config.kernel.dir.is_empty() {
        return Err(format!("{}: missing [kernel].dir", config_path.display()));
    }
    if config.kernel.default_step.is_empty() {
        return Err(format!(
            "{}: missing [kernel].default_step",
            config_path.display()
        ));
    }
    if config.userland.apps_dir.is_empty() {
        return Err(format!(
            "{}: missing [userland].apps_dir",
            config_path.display()
        ));
    }
    if config.disk.image.is_empty() {
        return Err(format!("{}: missing [disk].image", config_path.display()));
    }
    if let Some(size_mib) = config.disk.size_mib {
        if size_mib == 0 {
            return Err(format!(
                "{}: [disk].size_mib must be greater than 0",
                config_path.display()
            ));
        }
    }
    match config.disk.recreate.as_str() {
        "" | "never" | "if-missing" | "always" => {}
        other => {
            return Err(format!(
                "{}: [disk].recreate must be never, if-missing, or always (got '{}')",
                config_path.display(),
                other
            ));
        }
    }
    if !config.disk.partitions.is_empty() {
        let mut seen_indices = BTreeSet::new();
        let mut grow_count = 0usize;
        let mut sorted = config.disk.partitions.iter().collect::<Vec<_>>();
        sorted.sort_by_key(|partition| partition.index);
        for partition in &sorted {
            if partition.id.is_empty() {
                return Err(format!(
                    "{}: [[disk.partition]] is missing id",
                    config_path.display()
                ));
            }
            if partition.index == 0 {
                return Err(format!(
                    "{}: [[disk.partition]] '{}' has invalid index 0",
                    config_path.display(),
                    partition.id
                ));
            }
            if partition.index > 128 {
                return Err(format!(
                    "{}: [[disk.partition]] '{}' has invalid index {} (max 128)",
                    config_path.display(),
                    partition.id,
                    partition.index
                ));
            }
            if !seen_indices.insert(partition.index) {
                return Err(format!(
                    "{}: duplicate [[disk.partition]].index {}",
                    config_path.display(),
                    partition.index
                ));
            }
            if partition.format.is_empty() {
                return Err(format!(
                    "{}: [[disk.partition]] '{}' is missing format",
                    config_path.display(),
                    partition.id
                ));
            }
            if partition.grow {
                grow_count += 1;
            } else if partition.size_mib.unwrap_or(0) == 0 {
                return Err(format!(
                    "{}: [[disk.partition]] '{}' needs size_mib unless grow = true",
                    config_path.display(),
                    partition.id
                ));
            }
        }
        if grow_count > 1 {
            return Err(format!(
                "{}: at most one [[disk.partition]] may set grow = true",
                config_path.display()
            ));
        }
        if grow_count == 1 && !sorted.last().is_some_and(|partition| partition.grow) {
            return Err(format!(
                "{}: the grow = true partition must be last by index",
                config_path.display()
            ));
        }
    }
    if !config.startup_manifest.include.is_empty() && config.startup_manifest.path.is_empty() {
        return Err(format!(
            "{}: [startup_manifest].path is required when include is set",
            config_path.display()
        ));
    }
    Ok(())
}

fn validate_app_config(config: &AppConfig) -> Result<(), String> {
    if config.schema_version != 1 {
        return Err(format!(
            "{}: unsupported schema_version {}",
            config.config_path.display(),
            config.schema_version
        ));
    }
    if config.app.id.is_empty() {
        return Err(format!(
            "{}: missing [app].id",
            config.config_path.display()
        ));
    }
    if config.app.kind.is_empty() {
        return Err(format!(
            "{}: missing [app].kind",
            config.config_path.display()
        ));
    }
    if config.build.output_name.is_empty() {
        return Err(format!(
            "{}: missing [build].output_name",
            config.config_path.display()
        ));
    }
    match &config.source {
        SourceConfig::None => {}
        SourceConfig::Zig(src) if src.entry.is_empty() => {
            return Err(format!(
                "{}: missing [source.zig].entry",
                config.config_path.display()
            ));
        }
        SourceConfig::Cargo(src) if src.manifest.is_empty() || src.package.is_empty() => {
            return Err(format!(
                "{}: missing [source.cargo].manifest or package",
                config.config_path.display()
            ));
        }
        SourceConfig::Capc(src) if src.root.is_empty() || src.entry.is_empty() => {
            return Err(format!(
                "{}: missing [source.capc].root or entry",
                config.config_path.display()
            ));
        }
        SourceConfig::File(src) if src.path.is_empty() => {
            return Err(format!(
                "{}: missing [source.file].path",
                config.config_path.display()
            ));
        }
        SourceConfig::File(src) if !src.rebuild_tool.is_empty() && src.rebuild_dir.is_empty() => {
            return Err(format!(
                "{}: [source.file].rebuild_dir is required when rebuild_tool is set",
                config.config_path.display()
            ));
        }
        _ => {}
    }
    for entry in &config.publish {
        if !entry.path.starts_with('/') {
            return Err(format!(
                "{}: publish path must be absolute: {}",
                config.config_path.display(),
                entry.path
            ));
        }
        if entry.path.contains("..") {
            return Err(format!(
                "{}: publish path must not contain '..': {}",
                config.config_path.display(),
                entry.path
            ));
        }
    }
    if let Some(startup) = &config.startup {
        let Some(publish) = config
            .publish
            .iter()
            .find(|entry| entry.id == startup.publish)
        else {
            return Err(format!(
                "{}: startup.publish references missing publish entry '{}'",
                config.config_path.display(),
                startup.publish
            ));
        };
        if publish.fs != startup.load {
            return Err(format!(
                "{}: startup.load '{}' does not match publish fs '{}'",
                config.config_path.display(),
                startup.load,
                publish.fs
            ));
        }
        if startup.action == "block_driver" && startup.block.len() != 1 {
            return Err(format!(
                "{}: block_driver startup requires exactly one block selector",
                config.config_path.display()
            ));
        }
        if startup.action == "input_driver" && startup.input.len() != 1 {
            return Err(format!(
                "{}: input_driver startup requires exactly one input selector",
                config.config_path.display()
            ));
        }
        if startup.action == "console_driver" && startup.device.len() != 1 {
            return Err(format!(
                "{}: console_driver startup requires exactly one device selector",
                config.config_path.display()
            ));
        }
    }
    Ok(())
}

fn parse_document<F>(contents: &str, mut assign: F) -> Result<(), String>
where
    F: FnMut(&SectionPath, &str, Value) -> Result<(), String>,
{
    let mut current = SectionPath {
        parts: Vec::new(),
        is_array: false,
    };

    for (line_index, raw_line) in contents.lines().enumerate() {
        let line_number = line_index + 1;
        let line = strip_comments(raw_line).trim().to_string();
        if line.is_empty() {
            continue;
        }
        if line.starts_with("[[") && line.ends_with("]]") {
            current = parse_section_path(&line[2..line.len() - 2], true)
                .map_err(|err| format!("line {line_number}: {err}"))?;
            assign(&current, "", Value::String(String::new()))?;
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') {
            current = parse_section_path(&line[1..line.len() - 1], false)
                .map_err(|err| format!("line {line_number}: {err}"))?;
            continue;
        }
        let (key, value_src) =
            split_key_value(&line).map_err(|err| format!("line {line_number}: {err}"))?;
        let value = parse_value(value_src).map_err(|err| format!("line {line_number}: {err}"))?;
        assign(&current, key, value)?;
    }

    Ok(())
}

fn parse_section_path(input: &str, is_array: bool) -> Result<SectionPath, String> {
    let parts = input
        .split('.')
        .map(str::trim)
        .filter(|part| !part.is_empty())
        .map(ToOwned::to_owned)
        .collect::<Vec<_>>();
    if parts.is_empty() {
        return Err("empty section path".to_string());
    }
    Ok(SectionPath { parts, is_array })
}

fn split_key_value(line: &str) -> Result<(&str, &str), String> {
    let mut in_string = false;
    let mut escape = false;
    for (idx, ch) in line.char_indices() {
        if escape {
            escape = false;
            continue;
        }
        match ch {
            '\\' if in_string => escape = true,
            '"' => in_string = !in_string,
            '=' if !in_string => {
                let key = line[..idx].trim();
                let value = line[idx + 1..].trim();
                if key.is_empty() || value.is_empty() {
                    return Err("invalid key/value pair".to_string());
                }
                return Ok((key, value));
            }
            _ => {}
        }
    }
    Err("missing '='".to_string())
}

fn strip_comments(line: &str) -> String {
    let mut out = String::with_capacity(line.len());
    let mut in_string = false;
    let mut escape = false;
    for ch in line.chars() {
        if escape {
            out.push(ch);
            escape = false;
            continue;
        }
        match ch {
            '\\' if in_string => {
                out.push(ch);
                escape = true;
            }
            '"' => {
                out.push(ch);
                in_string = !in_string;
            }
            '#' if !in_string => break,
            _ => out.push(ch),
        }
    }
    out
}

fn parse_value(input: &str) -> Result<Value, String> {
    if input.starts_with('"') {
        return Ok(Value::String(parse_string(input)?));
    }
    if input == "true" {
        return Ok(Value::Bool(true));
    }
    if input == "false" {
        return Ok(Value::Bool(false));
    }
    if input.starts_with('[') {
        return Ok(Value::StringArray(parse_string_array(input)?));
    }
    let integer = input
        .parse::<i64>()
        .map_err(|_| format!("unsupported value: {input}"))?;
    Ok(Value::Integer(integer))
}

fn parse_string(input: &str) -> Result<String, String> {
    if !input.ends_with('"') || input.len() < 2 {
        return Err("unterminated string".to_string());
    }
    let inner = &input[1..input.len() - 1];
    let mut out = String::with_capacity(inner.len());
    let mut escape = false;
    for ch in inner.chars() {
        if escape {
            match ch {
                '\\' => out.push('\\'),
                '"' => out.push('"'),
                'n' => out.push('\n'),
                't' => out.push('\t'),
                _ => return Err(format!("unsupported escape: \\{ch}")),
            }
            escape = false;
            continue;
        }
        if ch == '\\' {
            escape = true;
        } else {
            out.push(ch);
        }
    }
    if escape {
        return Err("unterminated escape".to_string());
    }
    Ok(out)
}

fn parse_string_array(input: &str) -> Result<Vec<String>, String> {
    if !input.ends_with(']') {
        return Err("unterminated array".to_string());
    }
    let inner = input[1..input.len() - 1].trim();
    if inner.is_empty() {
        return Ok(Vec::new());
    }
    let mut items = Vec::new();
    let mut current = String::new();
    let mut in_string = false;
    let mut escape = false;
    for ch in inner.chars() {
        if escape {
            current.push(ch);
            escape = false;
            continue;
        }
        match ch {
            '\\' if in_string => {
                current.push(ch);
                escape = true;
            }
            '"' => {
                current.push(ch);
                in_string = !in_string;
            }
            ',' if !in_string => {
                items.push(parse_string(current.trim())?);
                current.clear();
            }
            _ => current.push(ch),
        }
    }
    if in_string {
        return Err("unterminated string in array".to_string());
    }
    if !current.trim().is_empty() {
        items.push(parse_string(current.trim())?);
    }
    Ok(items)
}

fn section_eq(section: &SectionPath, is_array: bool, parts: &[&str]) -> bool {
    if section.is_array != is_array || section.parts.len() != parts.len() {
        return false;
    }
    section
        .parts
        .iter()
        .map(String::as_str)
        .zip(parts.iter().copied())
        .all(|(lhs, rhs)| lhs == rhs)
}

fn assign_workspace_value(
    config: &mut WorkspaceConfig,
    section: &SectionPath,
    key: &str,
    value: Value,
) -> Result<(), String> {
    if section_eq(section, true, &["disk", "partition"]) && key.is_empty() {
        config.disk.partitions.push(DiskPartition::default());
        return Ok(());
    }
    if section_eq(section, true, &["publish_dir"]) && key.is_empty() {
        config.publish_dirs.push(PublishDir::default());
        return Ok(());
    }
    if !section.is_array && section.parts.is_empty() && key == "schema_version" {
        config.schema_version = value.into_u32("schema_version")?;
    } else if section_eq(section, false, &["workspace"]) && key == "name" {
        config.workspace.name = value.into_string("[workspace].name")?;
    } else if section_eq(section, false, &["workspace"]) && key == "root" {
        config.workspace.root = value.into_string("[workspace].root")?;
    } else if section_eq(section, false, &["artifacts"]) && key == "dir" {
        config.artifacts.dir = value.into_string("[artifacts].dir")?;
    } else if section_eq(section, false, &["artifacts"]) && key == "state_dir" {
        config.artifacts.state_dir = value.into_string("[artifacts].state_dir")?;
    } else if section_eq(section, false, &["kernel"]) && key == "dir" {
        config.kernel.dir = value.into_string("[kernel].dir")?;
    } else if section_eq(section, false, &["kernel"]) && key == "default_step" {
        config.kernel.default_step = value.into_string("[kernel].default_step")?;
    } else if section_eq(section, false, &["userland"]) && key == "apps_dir" {
        config.userland.apps_dir = value.into_string("[userland].apps_dir")?;
    } else if section_eq(section, false, &["userland"]) && key == "skip_kinds" {
        config.userland.skip_kinds = value.into_string_array("[userland].skip_kinds")?;
    } else if section_eq(section, false, &["userland"]) && key == "skip_apps" {
        config.userland.skip_apps = value.into_string_array("[userland].skip_apps")?;
    } else if section_eq(section, false, &["userland"])
        && key == "include_skipped_artifacts_in_manifests"
    {
        config.userland.include_skipped_artifacts_in_manifests =
            Some(value.into_bool("[userland].include_skipped_artifacts_in_manifests")?);
    } else if section_eq(section, false, &["disk"]) && key == "image" {
        config.disk.image = value.into_string("[disk].image")?;
    } else if section_eq(section, false, &["disk"]) && key == "size_mib" {
        config.disk.size_mib = Some(value.into_u32("[disk].size_mib")?);
    } else if section_eq(section, false, &["disk"]) && key == "recreate" {
        config.disk.recreate = value.into_string("[disk].recreate")?;
    } else if section_eq(section, true, &["disk", "partition"]) && key == "id" {
        last_partition_mut(config)?.id = value.into_string("[[disk.partition]].id")?;
    } else if section_eq(section, true, &["disk", "partition"]) && key == "index" {
        last_partition_mut(config)?.index = value.into_u32("[[disk.partition]].index")?;
    } else if section_eq(section, true, &["disk", "partition"]) && key == "format" {
        last_partition_mut(config)?.format = value.into_string("[[disk.partition]].format")?;
    } else if section_eq(section, true, &["disk", "partition"]) && key == "size_mib" {
        last_partition_mut(config)?.size_mib = Some(value.into_u32("[[disk.partition]].size_mib")?);
    } else if section_eq(section, true, &["disk", "partition"]) && key == "grow" {
        last_partition_mut(config)?.grow = value.into_bool("[[disk.partition]].grow")?;
    } else if section_eq(section, false, &["manifests"]) && key == "dir" {
        config.manifests.dir = value.into_string("[manifests].dir")?;
    } else if section_eq(section, false, &["manifests"]) && key == "bootfs" {
        config.manifests.bootfs = value.into_string("[manifests].bootfs")?;
    } else if section_eq(section, false, &["manifests"]) && key == "rootfs" {
        config.manifests.rootfs = value.into_string("[manifests].rootfs")?;
    } else if section_eq(section, false, &["manifests"]) && key == "startup" {
        config.manifests.startup = value.into_string("[manifests].startup")?;
    } else if section_eq(section, true, &["publish_dir"]) && key == "fs" {
        last_publish_dir_mut(config)?.fs = value.into_string("[[publish_dir]].fs")?;
    } else if section_eq(section, true, &["publish_dir"]) && key == "path" {
        last_publish_dir_mut(config)?.path = value.into_string("[[publish_dir]].path")?;
    } else if section_eq(section, false, &["startup_manifest"]) && key == "path" {
        config.startup_manifest.path = value.into_string("[startup_manifest].path")?;
    } else if section_eq(section, false, &["startup_manifest"]) && key == "include" {
        config.startup_manifest.include = value.into_string_array("[startup_manifest].include")?;
    } else if section_eq(section, false, &["toolchain"]) && key == "zig" {
        config.toolchain.zig = value.into_string("[toolchain].zig")?;
    } else if section_eq(section, false, &["toolchain"]) && key == "cargo" {
        config.toolchain.cargo = value.into_string("[toolchain].cargo")?;
    } else if section_eq(section, false, &["toolchain"]) && key == "pwsh" {
        config.toolchain.pwsh = value.into_string("[toolchain].pwsh")?;
    } else if section_eq(section, false, &["run"]) && key == "default" {
        config.run.default = value.into_string("[run].default")?;
    } else if !section.is_array
        && section.parts.len() == 3
        && section.parts[0] == "run"
        && section.parts[1] == "profile"
        && key == "kind"
    {
        config
            .run
            .profiles
            .entry(section.parts[2].clone())
            .or_default()
            .kind = value.into_string("[run.profile.*].kind")?;
    } else if !section.is_array
        && section.parts.len() == 3
        && section.parts[0] == "run"
        && section.parts[1] == "profile"
        && key == "path"
    {
        config
            .run
            .profiles
            .entry(section.parts[2].clone())
            .or_default()
            .path = value.into_string("[run.profile.*].path")?;
    }
    Ok(())
}

pub fn app_kind_is_skipped(workspace: &WorkspaceConfig, kind: &str) -> bool {
    workspace
        .userland
        .skip_kinds
        .iter()
        .any(|item| item.eq_ignore_ascii_case(kind))
}

pub fn app_id_is_skipped(workspace: &WorkspaceConfig, app_id: &str) -> bool {
    workspace
        .userland
        .skip_apps
        .iter()
        .any(|item| item.eq_ignore_ascii_case(app_id))
}

pub fn app_is_skipped(workspace: &WorkspaceConfig, app: &AppConfig) -> bool {
    app_kind_is_skipped(workspace, &app.app.kind) || app_id_is_skipped(workspace, &app.app.id)
}

pub fn include_skipped_artifacts_in_manifests(workspace: &WorkspaceConfig) -> bool {
    workspace
        .userland
        .include_skipped_artifacts_in_manifests
        .unwrap_or(true)
}

fn assign_app_value(
    config: &mut AppConfig,
    section: &SectionPath,
    key: &str,
    value: Value,
) -> Result<(), String> {
    if section_eq(section, true, &["publish"]) && key.is_empty() {
        config.publish.push(PublishEntry::default());
        return Ok(());
    }
    if !section.is_array && section.parts.is_empty() && key == "schema_version" {
        config.schema_version = value.into_u32("schema_version")?;
    } else if section_eq(section, false, &["app"]) && key == "id" {
        config.app.id = value.into_string("[app].id")?;
    } else if section_eq(section, false, &["app"]) && key == "kind" {
        config.app.kind = value.into_string("[app].kind")?;
    } else if section_eq(section, false, &["app"]) && key == "role" {
        config.app.role = value.into_string("[app].role")?;
    } else if section_eq(section, false, &["build"]) && key == "format" {
        config.build.format = value.into_string("[build].format")?;
    } else if section_eq(section, false, &["build"]) && key == "target" {
        config.build.target = value.into_string("[build].target")?;
    } else if section_eq(section, false, &["build"]) && key == "optimize" {
        config.build.optimize = value.into_string("[build].optimize")?;
    } else if section_eq(section, false, &["build"]) && key == "output_name" {
        config.build.output_name = value.into_string("[build].output_name")?;
    } else if section_eq(section, false, &["build"]) && key == "strip" {
        config.build.strip = value.into_bool("[build].strip")?;
    } else if section_eq(section, false, &["source", "zig"]) && key == "entry" {
        ensure_zig_source(&mut config.source).entry = value.into_string("[source.zig].entry")?;
    } else if section_eq(section, false, &["source", "zig"]) && key == "module" {
        ensure_zig_source(&mut config.source).module = value.into_string("[source.zig].module")?;
    } else if section_eq(section, false, &["source", "zig"]) && key == "imports" {
        ensure_zig_source(&mut config.source).imports =
            value.into_string_array("[source.zig].imports")?;
    } else if section_eq(section, false, &["source", "zig"]) && key == "c_sources" {
        ensure_zig_source(&mut config.source).c_sources =
            value.into_string_array("[source.zig].c_sources")?;
    } else if section_eq(section, false, &["source", "zig"]) && key == "include_dirs" {
        ensure_zig_source(&mut config.source).include_dirs =
            value.into_string_array("[source.zig].include_dirs")?;
    } else if section_eq(section, false, &["source", "zig"]) && key == "cflags" {
        ensure_zig_source(&mut config.source).cflags =
            value.into_string_array("[source.zig].cflags")?;
    } else if section_eq(section, false, &["source", "cargo"]) && key == "manifest" {
        ensure_cargo_source(&mut config.source).manifest =
            value.into_string("[source.cargo].manifest")?;
    } else if section_eq(section, false, &["source", "cargo"]) && key == "package" {
        ensure_cargo_source(&mut config.source).package =
            value.into_string("[source.cargo].package")?;
    } else if section_eq(section, false, &["source", "cargo"]) && key == "bin" {
        ensure_cargo_source(&mut config.source).bin = value.into_string("[source.cargo].bin")?;
    } else if section_eq(section, false, &["source", "cargo"]) && key == "target" {
        ensure_cargo_source(&mut config.source).target =
            value.into_string("[source.cargo].target")?;
    } else if section_eq(section, false, &["source", "cargo"]) && key == "target_dir" {
        ensure_cargo_source(&mut config.source).target_dir =
            value.into_string("[source.cargo].target_dir")?;
    } else if section_eq(section, false, &["source", "capc"]) && key == "root" {
        ensure_capc_source(&mut config.source).root = value.into_string("[source.capc].root")?;
    } else if section_eq(section, false, &["source", "capc"]) && key == "entry" {
        ensure_capc_source(&mut config.source).entry = value.into_string("[source.capc].entry")?;
    } else if section_eq(section, false, &["source", "capc"]) && key == "crt" {
        ensure_capc_source(&mut config.source).crt = value.into_string("[source.capc].crt")?;
    } else if section_eq(section, false, &["source", "capc"]) && key == "sources" {
        ensure_capc_source(&mut config.source).sources =
            value.into_string_array("[source.capc].sources")?;
    } else if section_eq(section, false, &["source", "capc"]) && key == "include_dirs" {
        ensure_capc_source(&mut config.source).include_dirs =
            value.into_string_array("[source.capc].include_dirs")?;
    } else if section_eq(section, false, &["source", "file"]) && key == "path" {
        ensure_file_source(&mut config.source).path = value.into_string("[source.file].path")?;
    } else if section_eq(section, false, &["source", "file"]) && key == "rebuild_tool" {
        ensure_file_source(&mut config.source).rebuild_tool =
            value.into_string("[source.file].rebuild_tool")?;
    } else if section_eq(section, false, &["source", "file"]) && key == "rebuild_dir" {
        ensure_file_source(&mut config.source).rebuild_dir =
            value.into_string("[source.file].rebuild_dir")?;
    } else if section_eq(section, false, &["source", "file"]) && key == "rebuild_args" {
        ensure_file_source(&mut config.source).rebuild_args =
            value.into_string_array("[source.file].rebuild_args")?;
    } else if section_eq(section, true, &["publish"]) && key == "id" {
        last_publish_mut(config)?.id = value.into_string("[[publish]].id")?;
    } else if section_eq(section, true, &["publish"]) && key == "fs" {
        last_publish_mut(config)?.fs = value.into_string("[[publish]].fs")?;
    } else if section_eq(section, true, &["publish"]) && key == "path" {
        last_publish_mut(config)?.path = value.into_string("[[publish]].path")?;
    } else if section_eq(section, false, &["startup"]) && key == "publish" {
        ensure_startup(config).publish = value.into_string("[startup].publish")?;
    } else if section_eq(section, false, &["startup"]) && key == "action" {
        ensure_startup(config).action = value.into_string("[startup].action")?;
    } else if section_eq(section, false, &["startup"]) && key == "name" {
        ensure_startup(config).name = value.into_string("[startup].name")?;
    } else if section_eq(section, false, &["startup"]) && key == "label" {
        ensure_startup(config).label = value.into_string("[startup].label")?;
    } else if section_eq(section, false, &["startup"]) && key == "load" {
        ensure_startup(config).load = value.into_string("[startup].load")?;
    } else if section_eq(section, false, &["startup"]) && key == "after" {
        ensure_startup(config).after = value.into_string_array("[startup].after")?;
    } else if section_eq(section, false, &["startup"]) && key == "requires" {
        ensure_startup(config).requires = value.into_string_array("[startup].requires")?;
    } else if section_eq(section, false, &["startup"]) && key == "provides" {
        ensure_startup(config).provides = value.into_string_array("[startup].provides")?;
    } else if section_eq(section, false, &["startup"]) && key == "ensure" {
        ensure_startup(config).ensure = value.into_string_array("[startup].ensure")?;
    } else if section_eq(section, false, &["startup"]) && key == "block" {
        ensure_startup(config).block = value.into_string_array("[startup].block")?;
    } else if section_eq(section, false, &["startup"]) && key == "device" {
        ensure_startup(config).device = value.into_string_array("[startup].device")?;
    } else if section_eq(section, false, &["startup"]) && key == "input" {
        ensure_startup(config).input = value.into_string_array("[startup].input")?;
    }
    Ok(())
}

fn last_partition_mut(config: &mut WorkspaceConfig) -> Result<&mut DiskPartition, String> {
    config
        .disk
        .partitions
        .last_mut()
        .ok_or_else(|| "[[disk.partition]] must appear before its fields".to_string())
}

fn last_publish_dir_mut(config: &mut WorkspaceConfig) -> Result<&mut PublishDir, String> {
    config
        .publish_dirs
        .last_mut()
        .ok_or_else(|| "[[publish_dir]] must appear before its fields".to_string())
}

fn last_publish_mut(config: &mut AppConfig) -> Result<&mut PublishEntry, String> {
    config
        .publish
        .last_mut()
        .ok_or_else(|| "[[publish]] must appear before its fields".to_string())
}

fn ensure_startup(config: &mut AppConfig) -> &mut StartupConfig {
    config.startup.get_or_insert_with(StartupConfig::default)
}

fn ensure_zig_source(source: &mut SourceConfig) -> &mut ZigSource {
    if !matches!(source, SourceConfig::Zig(_)) {
        *source = SourceConfig::Zig(ZigSource::default());
    }
    match source {
        SourceConfig::Zig(value) => value,
        _ => unreachable!(),
    }
}

fn ensure_cargo_source(source: &mut SourceConfig) -> &mut CargoSource {
    if !matches!(source, SourceConfig::Cargo(_)) {
        *source = SourceConfig::Cargo(CargoSource::default());
    }
    match source {
        SourceConfig::Cargo(value) => value,
        _ => unreachable!(),
    }
}

fn ensure_capc_source(source: &mut SourceConfig) -> &mut CapcSource {
    if !matches!(source, SourceConfig::Capc(_)) {
        *source = SourceConfig::Capc(CapcSource::default());
    }
    match source {
        SourceConfig::Capc(value) => value,
        _ => unreachable!(),
    }
}

fn ensure_file_source(source: &mut SourceConfig) -> &mut FileSource {
    if !matches!(source, SourceConfig::File(_)) {
        *source = SourceConfig::File(FileSource::default());
    }
    match source {
        SourceConfig::File(value) => value,
        _ => unreachable!(),
    }
}
