use super::Extractor;
use anyhow::{Context, Result};
use std::path::{Path, PathBuf};

pub struct SevenZExtractor;

impl Extractor for SevenZExtractor {
    fn can_handle(&self, ext: &str) -> bool {
        ext == "7z"
    }

    fn extract(&self, archive_path: &Path, target_dir: &Path) -> Result<Vec<PathBuf>> {
        sevenz_rust2::decompress_file(archive_path, target_dir).context("Failed to read 7z archive")?;
        Ok(super::list_files(target_dir))
    }
}
