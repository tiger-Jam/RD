use super::Extractor;
use anyhow::{Context, Result};
use std::fs;
use std::path::{Path, PathBuf};

pub struct ChartExtractor;

impl Extractor for ChartExtractor {
    fn can_handle(&self, ext: &str) -> bool {
        matches!(ext, "bms" | "bme" | "bml" | "pms" | "bmson")
    }

    fn extract(&self, chart_path: &Path, target_dir: &Path) -> Result<Vec<PathBuf>> {
        let file_name = chart_path.file_name().context("Invalid filename")?;
        let target_path = target_dir.join(file_name);

        fs::rename(chart_path, &target_path)?;

        Ok(vec![target_path])
    }
}
