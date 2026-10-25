use anyhow::{Context, Result};
use log::warn;
use std::fs;
use std::path::{Path, PathBuf};

pub mod chart;
pub mod exe;
pub mod lzh;
pub mod rar;
pub mod sevenz;
pub mod zip;

pub trait Extractor: Send + Sync {
    fn can_handle(&self, ext: &str) -> bool;
    fn extract(&self, archive_path: &Path, target_dir: &Path) -> Result<Vec<PathBuf>>;

    fn extract_to(&self, path: &Path) -> Result<ExtractResult> {
        let target_dir = path.with_extension("");

        if !target_dir.exists() {
            fs::create_dir_all(&target_dir)?;
        }

        let extracted_paths = self.extract(path, &target_dir)?;
        cleanup_archive(path);

        Ok(ExtractResult {
            archive_path: path.to_path_buf(),
            target_dir,
            extracted_paths,
        })
    }
}

const EXTRACTORS: &[&dyn Extractor] = &[
    &zip::ZipExtractor,
    &rar::RarExtractor,
    &sevenz::SevenZExtractor,
    &lzh::LzhExtractor,
    &exe::ExeExtractor,
    &chart::ChartExtractor,
];

/// A name stored as raw bytes: UTF-8 if it is valid, else Shift_JIS (Japanese archives rarely say which).
pub fn decode_name(raw: &[u8]) -> String {
    match std::str::from_utf8(raw) {
        Ok(s) => s.to_string(),
        Err(_) => encoding_rs::SHIFT_JIS.decode(raw).0.into_owned(),
    }
}

/// `name` under `dir`, refusing absolute paths and `..` (an archive must not write outside its folder).
pub fn safe_join(dir: &Path, name: &str) -> Option<PathBuf> {
    let mut out = dir.to_path_buf();
    for part in name.split(['/', '\\']) {
        match part {
            "" | "." => {}
            ".." => return None,
            p if p.contains(':') => return None,
            p => out.push(p),
        }
    }
    (out != dir).then_some(out)
}

/// Every file under `dir` (for extractors that do not report what they wrote).
pub fn list_files(dir: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        let Ok(rd) = fs::read_dir(&d) else { continue };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() { stack.push(p) } else { out.push(p) }
        }
    }
    out
}

#[derive(Debug)]
pub struct ExtractResult {
    pub archive_path: PathBuf,
    pub target_dir: PathBuf,
    pub extracted_paths: Vec<PathBuf>,
}

pub fn find_extractor(path: &Path) -> Result<&'static dyn Extractor> {
    let extension = path
        .extension()
        .and_then(|e| e.to_str())
        .context("Invalid extension")?
        .to_lowercase();

    EXTRACTORS
        .iter()
        .copied()
        .find(|e| e.can_handle(&extension))
        .context("No extractor found")
}

pub fn extract(path: &Path) -> Result<ExtractResult> {
    find_extractor(path)?.extract_to(path)
}

fn cleanup_archive(path: &Path) {
    if path.is_file()
        && let Err(e) = fs::remove_file(path)
    {
        warn!("Failed to remove archive: {} - {}", path.display(), e);
    }
}
