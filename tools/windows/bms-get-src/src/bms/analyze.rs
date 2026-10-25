use anyhow::Result;
use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};

pub fn find_chart(root: &Path, md5: &str) -> Result<Option<PathBuf>> {
    if root.is_file() {
        return if is_chart(root) && file_md5(root)?.eq_ignore_ascii_case(md5) {
            Ok(Some(root.to_path_buf()))
        } else {
            Ok(None)
        };
    }

    if root.is_dir() {
        for entry in fs::read_dir(root)? {
            if let Some(chart) = find_chart(&entry?.path(), md5)? {
                return Ok(Some(chart));
            }
        }
    }

    Ok(None)
}

pub fn chart_refs(path: &Path) -> Result<HashSet<PathBuf>> {
    // Charts are mostly Shift_JIS; the file names they reference must be read the same way as the archive's names
    let text = crate::extract::decode_name(&fs::read(path)?);
    let mut refs = HashSet::new();

    for line in text.lines() {
        let Some((key, file_name)) = split_command(line.trim()) else {
            continue;
        };

        let key = key.to_ascii_uppercase();
        if key.starts_with("#WAV") || key.starts_with("#BMP") {
            refs.insert(PathBuf::from(file_name.replace('\\', "/")));
        }
    }

    Ok(refs)
}

fn split_command(line: &str) -> Option<(&str, &str)> {
    let index = line.find(char::is_whitespace)?;
    let value = line[index..].trim();

    if value.is_empty() {
        None
    } else {
        Some((&line[..index], value))
    }
}

fn is_chart(path: &Path) -> bool {
    path.extension()
        .and_then(|ext| ext.to_str())
        .is_some_and(|ext| matches!(ext, "bms" | "bme" | "bml" | "pms"))
}

fn file_md5(path: &Path) -> Result<String> {
    Ok(format!("{:x}", md5::compute(fs::read(path)?)))
}
