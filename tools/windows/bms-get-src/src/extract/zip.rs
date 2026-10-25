use super::{Extractor, decode_name, safe_join};
use anyhow::{Context, Result};
use std::fs;
use std::path::{Path, PathBuf};

pub struct ZipExtractor;

impl Extractor for ZipExtractor {
    fn can_handle(&self, ext: &str) -> bool {
        ext == "zip"
    }

    fn extract(&self, archive_path: &Path, target_dir: &Path) -> Result<Vec<PathBuf>> {
        let file = fs::File::open(archive_path).context("Failed to open archive")?;
        let mut archive = zip::ZipArchive::new(file).context("Failed to read zip archive")?;
        let mut extracted_paths = Vec::new();

        for i in 0..archive.len() {
            let mut file = archive.by_index(i)?;
            // Japanese zips store names in Shift_JIS without saying so: decode the raw name, not the CP437 guess
            let name = decode_name(file.name_raw());
            let Some(outpath) = safe_join(target_dir, &name) else { continue };

            if file.is_dir() || name.ends_with('/') {
                fs::create_dir_all(&outpath)?;
            } else {
                if let Some(p) = outpath.parent() {
                    fs::create_dir_all(p)?;
                }
                let mut outfile = fs::File::create(&outpath)?;
                std::io::copy(&mut file, &mut outfile)?;
            }
            extracted_paths.push(outpath);
        }
        Ok(extracted_paths)
    }
}
