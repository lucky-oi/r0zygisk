mod apatch;
mod kernelsu;
mod magisk;
use anyhow::{bail, Result};
use std::fs;
use std::process::{Command, Stdio};

const EXTRA_DENYLIST_PATH: &str = "/data/adb/zygisksu/denylist";
const DEFAULT_EXTRA_DENYLIST: &[&str] = &[
    "com.globe.gcash.android",
    "io.github.vvb2060.mahoshojo",
    "com.xff.launch",
];

#[derive(Debug)]
pub enum RootImpl {
    None,
    TooOld,
    Abnormal,
    Multiple,
    APatch,
    KernelSU,
    Magisk,
}

static mut ROOT_IMPL: RootImpl = RootImpl::None;

pub fn setup() {
    let ksu_version = kernelsu::get_kernel_su();
    let magisk_version = magisk::get_magisk();
    let apatch_version = apatch::get_apatch();

    let impl_ = match (&ksu_version, &magisk_version, &apatch_version) {
        (Some(kernelsu::Version::Supported), _, _) => RootImpl::KernelSU,
        (_, _, Some(apatch::Version::Supported)) => RootImpl::APatch,
        (_, Some(magisk::Version::Supported), _) => RootImpl::Magisk,
        (Some(kernelsu::Version::TooOld), _, _) | (_, Some(magisk::Version::TooOld), _) => {
            RootImpl::TooOld
        }
        (None, None, None) => RootImpl::None,
    };
    unsafe {
        ROOT_IMPL = impl_;
    }
}

pub fn get_impl() -> &'static RootImpl {
    unsafe { &ROOT_IMPL }
}

pub fn uid_granted_root(uid: i32) -> bool {
    match get_impl() {
        RootImpl::APatch => apatch::uid_granted_root(uid),
        RootImpl::KernelSU => kernelsu::uid_granted_root(uid),
        RootImpl::Magisk => magisk::uid_granted_root(uid),
        _ => false,
    }
}

pub fn uid_should_umount(uid: i32) -> bool {
    if root_uid_should_umount(uid) {
        return true;
    }
    uid_on_extra_denylist(uid)
}

pub fn process_should_umount(uid: i32, process: &str) -> bool {
    if root_uid_should_umount(uid) {
        return true;
    }
    uid_on_extra_denylist(uid) || process_on_extra_denylist(process)
}

fn root_uid_should_umount(uid: i32) -> bool {
    match get_impl() {
        RootImpl::APatch => apatch::uid_should_umount(uid),
        RootImpl::KernelSU => kernelsu::uid_should_umount(uid),
        RootImpl::Magisk => magisk::uid_should_umount(uid),
        _ => false,
    }
}

pub fn uid_should_force_hide(uid: i32) -> bool {
    uid_on_extra_denylist(uid)
}

pub fn process_should_force_hide(uid: i32, process: &str) -> bool {
    uid_on_extra_denylist(uid) || process_on_extra_denylist(process)
}

pub fn uid_is_manager(uid: i32) -> bool {
    match get_impl() {
        RootImpl::APatch => apatch::uid_is_manager(uid),
        RootImpl::KernelSU => kernelsu::uid_is_manager(uid),
        RootImpl::Magisk => magisk::uid_is_manager(uid),
        _ => false,
    }
}

pub fn set_package_exclude(pkg: &str, exclude: bool) -> Result<()> {
    match get_impl() {
        RootImpl::APatch => apatch::set_package_exclude(pkg, exclude),
        _ => bail!("current root impl is not APatch: {:?}", get_impl()),
    }
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

fn read_extra_denylist() -> Vec<String> {
    let mut entries: Vec<String> = fs::read_to_string(EXTRA_DENYLIST_PATH)
        .unwrap_or_default()
        .lines()
        .filter_map(|line| {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                None
            } else {
                Some(line.to_string())
            }
        })
        .collect();
    for pkg in DEFAULT_EXTRA_DENYLIST {
        if !entries.iter().any(|entry| entry == pkg) {
            entries.push((*pkg).to_string());
        }
    }
    entries
}

fn uid_on_extra_denylist(uid: i32) -> bool {
    let packages = packages_for_uid(uid);
    if packages.is_empty() {
        return false;
    }
    let denylist = read_extra_denylist();
    if denylist.is_empty() {
        return false;
    }
    packages.iter().any(|pkg| denylist.contains(pkg))
}

fn process_on_extra_denylist(process: &str) -> bool {
    let process = process.trim_matches(char::from(0)).trim();
    if process.is_empty() {
        return false;
    }
    read_extra_denylist()
        .iter()
        .any(|pkg| process == pkg || process.starts_with(&format!("{pkg}:")))
}
