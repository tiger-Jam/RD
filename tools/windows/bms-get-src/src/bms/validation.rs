use anyhow::Result;
use std::path::{Path, PathBuf};

use super::analyze::{chart_refs, find_chart};

const MIN_REF_MATCH_PERCENT: usize = 90;

pub fn validate_md5(md5: &str, root: &Path) -> Result<bool> {
    Ok(find_chart(root, md5)?.is_some())
}

pub fn validate_ref(md5: &str, root: &Path) -> Result<bool> {
    let Some(chart) = find_chart(root, md5)? else {
        return Ok(false);
    };

    let refs = chart_refs(&chart)?;
    let root = chart.parent().unwrap_or(&chart);
    let found = refs
        .iter()
        .filter(|file| resolve_ref_path(&root.join(file)).is_some())
        .count();

    Ok(found * 100 >= refs.len() * MIN_REF_MATCH_PERCENT)
}

/// Like `validate_ref`, but a referenced file may also be in `extra` (the existing body folder).
pub fn validate_ref_with(md5: &str, root: &Path, extra: &Path) -> Result<bool> {
    let Some(chart) = find_chart(root, md5)? else {
        return Ok(false);
    };

    let refs = chart_refs(&chart)?;
    let dir = chart.parent().unwrap_or(&chart);
    let found = refs
        .iter()
        .filter(|file| resolve_ref_path(&dir.join(file)).is_some() || resolve_ref_path(&extra.join(file)).is_some())
        .count();

    Ok(found * 100 >= refs.len() * MIN_REF_MATCH_PERCENT)
}

fn resolve_ref_path(path: &Path) -> Option<PathBuf> {
    if path.exists() {
        return Some(path.to_path_buf());
    }

    // Players load the same sound or picture under another extension (a wav that was converted to ogg, and so on)
    let ext = path.extension().and_then(|e| e.to_str()).map(|e| e.to_ascii_lowercase()).unwrap_or_default();
    let alternatives: &[&str] = match ext.as_str() {
        "wav" | "ogg" | "flac" | "mp3" => &["wav", "ogg", "flac", "mp3"],
        "bmp" | "png" | "jpg" | "jpeg" | "gif" => &["bmp", "png", "jpg", "jpeg", "gif"],
        "mpg" | "mpeg" | "avi" | "wmv" | "mp4" | "webm" => &["mpg", "mpeg", "avi", "wmv", "mp4", "webm"],
        _ => &[],
    };
    alternatives.iter().map(|e| path.with_extension(e)).find(|p| p.exists())
}

/// Share of the chart's files found under `dir` (0..=100).
pub fn ref_match_in(chart: &Path, dir: &Path) -> Result<usize> {
    let refs = chart_refs(chart)?;
    if refs.is_empty() {
        return Ok(100);
    }
    let found = refs.iter().filter(|file| resolve_ref_path(&dir.join(file)).is_some()).count();
    Ok(found * 100 / refs.len())
}

pub const MIN_MATCH: usize = MIN_REF_MATCH_PERCENT;
