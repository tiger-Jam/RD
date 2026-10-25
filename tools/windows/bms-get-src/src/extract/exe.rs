//! Self-extracting archives (.exe): an LZH, zip, rar or 7z inside. The archive part is cut out and extracted;
//! the program itself is never run.

use super::Extractor;
use crate::sniff;
use anyhow::{Result, bail};
use std::fs;
use std::path::{Path, PathBuf};

pub struct ExeExtractor;

impl Extractor for ExeExtractor {
    fn can_handle(&self, ext: &str) -> bool {
        ext == "exe"
    }

    fn extract(&self, archive_path: &Path, target_dir: &Path) -> Result<Vec<PathBuf>> {
        let data = fs::read(archive_path)?;
        // zip readers find the directory from the end, so a zip SFX opens as it is
        if data.windows(4).any(|w| w == b"PK\x05\x06") {
            let tmp = target_dir.with_extension("sfx.zip");
            fs::copy(archive_path, &tmp)?;
            let r = super::zip::ZipExtractor.extract(&tmp, target_dir);
            let _ = fs::remove_file(&tmp);
            if r.is_ok() {
                return r;
            }
        }
        let start = 0x200.min(data.len());
        for i in start..data.len().saturating_sub(8) {
            let (ext, hit) = if sniff::is_lzh_at(&data, i) {
                ("lzh", true)
            } else if data[i..].starts_with(b"Rar!\x1a\x07") {
                ("rar", true)
            } else if data[i..].starts_with(b"7z\xbc\xaf\x27\x1c") {
                ("7z", true)
            } else {
                ("", false)
            };
            if !hit {
                continue;
            }
            let tmp = target_dir.with_extension(format!("sfx.{}", ext));
            fs::write(&tmp, &data[i..])?;
            let r = super::find_extractor(&tmp).and_then(|e| e.extract(&tmp, target_dir));
            let _ = fs::remove_file(&tmp);
            if let Ok(paths) = r {
                if !paths.is_empty() {
                    return Ok(paths);
                }
            }
        }
        bail!("No archive inside the exe")
    }
}
