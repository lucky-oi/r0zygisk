use std::fs;
use std::os::android::fs::MetadataExt;
use std::path::Path;
use std::process::{Command, Stdio};
use anyhow::Result;

const APATCH_WORK_DIR: &str = "/data/adb/ap";
const APATCH_DAEMON_PATH: &str = "/data/adb/apd";
const APATCH_PACKAGE_CONFIG: &str = "/data/adb/ap/package_config";
const APATCH_MANAGER_PACKAGE: &str = "me.bmax.apatch";

pub enum Version {
    Supported,
}

#[derive(Debug, Clone)]
struct PackageConfig {
    pkg: String,
    exclude: i32,
    allow: i32,
    uid: i32,
    to_uid: i32,
    sctx: String,
}

pub fn get_apatch() -> Option<Version> {
    if Path::new(APATCH_DAEMON_PATH).exists() || Path::new(APATCH_WORK_DIR).exists() {
        Some(Version::Supported)
    } else {
        None
    }
}

pub fn uid_granted_root(uid: i32) -> bool {
    let packages = packages_for_uid(uid);
    if packages.is_empty() {
        return false;
    }
    read_package_configs().into_iter().any(|config| {
        packages.iter().any(|pkg| pkg == &config.pkg)
            && config.allow != 0
            && same_app_id(config.uid, uid)
    })
}

pub fn uid_should_umount(uid: i32) -> bool {
    let packages = packages_for_uid(uid);
    if packages.is_empty() {
        return false;
    }
    read_package_configs().into_iter().any(|config| {
        packages.iter().any(|pkg| pkg == &config.pkg)
            && config.exclude != 0
            && same_app_id(config.uid, uid)
    })
}

pub fn uid_is_manager(uid: i32) -> bool {
    [
        format!("/data/user_de/0/{APATCH_MANAGER_PACKAGE}"),
        format!("/data/user/0/{APATCH_MANAGER_PACKAGE}"),
    ]
    .into_iter()
    .any(|path| {
        fs::metadata(path)
            .map(|meta| meta.st_uid() == uid as u32)
            .unwrap_or(false)
    })
}

pub fn set_package_exclude(pkg: &str, exclude: bool) -> Result<()> {
    let mut configs = read_package_configs();

    if let Some(config) = configs.iter_mut().find(|config| config.pkg == pkg) {
        config.exclude = if exclude { 1 } else { 0 };
    } else if exclude {
        let uid = resolve_package_uid(pkg).ok_or_else(|| anyhow::anyhow!("无法解析 {pkg} 的 uid"))?;
        configs.push(PackageConfig {
            pkg: pkg.to_string(),
            exclude: 1,
            allow: 0,
            uid,
            to_uid: 0,
            sctx: "u:r:untrusted_app:s0".to_string(),
        });
    } else {
        return Ok(());
    }

    configs.retain(|config| config.exclude != 0 || config.allow != 0);
    write_package_configs(&configs)
}

fn packages_for_uid(uid: i32) -> Vec<String> {
    Command::new("pm")
        .args(["list", "packages", "--uid", &uid.to_string()])
        .stdout(Stdio::piped())
        .spawn()
        .ok()
        .and_then(|child| child.wait_with_output().ok())
        .and_then(|output| String::from_utf8(output.stdout).ok())
        .map(|output| {
            output
                .lines()
                .filter_map(|line| line.strip_prefix("package:"))
                .filter_map(|line| line.split_whitespace().next())
                .map(|pkg| pkg.to_string())
                .collect()
        })
        .unwrap_or_default()
}

fn resolve_package_uid(pkg: &str) -> Option<i32> {
    Command::new("pm")
        .args(["list", "packages", "-U", pkg])
        .stdout(Stdio::piped())
        .spawn()
        .ok()
        .and_then(|child| child.wait_with_output().ok())
        .and_then(|output| String::from_utf8(output.stdout).ok())
        .and_then(|output| {
            output
                .lines()
                .find_map(|line| line.strip_prefix(&format!("package:{pkg} uid:")))
                .and_then(|uid| uid.trim().parse::<i32>().ok())
        })
}

fn read_package_configs() -> Vec<PackageConfig> {
    fs::read_to_string(APATCH_PACKAGE_CONFIG)
        .ok()
        .map(|raw| raw.lines().filter_map(parse_package_config).collect())
        .unwrap_or_default()
}

fn parse_package_config(line: &str) -> Option<PackageConfig> {
    let line = line.trim();
    if line.is_empty() {
        return None;
    }
    let mut parts = line.split(',');
    let pkg = parts.next()?.trim().to_string();
    let exclude = parts.next()?.trim().parse().ok()?;
    let allow = parts.next()?.trim().parse().ok()?;
    let uid = parts.next()?.trim().parse().ok()?;
    let _to_uid = parts.next()?.trim().parse::<i32>().ok()?;
    let sctx = parts.next()?.trim();
    Some(PackageConfig {
        pkg,
        exclude,
        allow,
        uid,
        to_uid: _to_uid,
        sctx: sctx.to_string(),
    })
}

fn write_package_configs(configs: &[PackageConfig]) -> Result<()> {
    let mut lines = Vec::with_capacity(configs.len() + 1);
    lines.push("pkg,exclude,allow,uid,to_uid,sctx".to_string());
    for config in configs {
        if config.exclude == 0 && config.allow == 0 {
            continue;
        }
        lines.push(format!(
            "{},{},{},{},{},{}",
            config.pkg, config.exclude, config.allow, config.uid, config.to_uid, config.sctx
        ));
    }
    let content = format!("{}\n", lines.join("\n"));
    fs::write(APATCH_PACKAGE_CONFIG, content).map_err(Into::into)
}

fn same_app_id(lhs: i32, rhs: i32) -> bool {
    lhs.rem_euclid(100_000) == rhs.rem_euclid(100_000)
}
