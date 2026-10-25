use anyhow::Result;
use log::warn;
use std::fs;
use std::path::{Path, PathBuf};

pub fn merge_bms_dir(target: &Path, source: &Path) -> Result<()> {
    let root = source_root(source)?;
    merge_dir(&root, target)?;
    fs::remove_dir_all(source)?;
    Ok(())
}

fn source_root(source: &Path) -> Result<PathBuf> {
    let mut entries = fs::read_dir(source)?;
    let Some(first) = entries.next().transpose()? else {
        return Ok(source.to_path_buf());
    };

    if entries.next().is_none() && first.path().is_dir() {
        Ok(first.path())
    } else {
        Ok(source.to_path_buf())
    }
}

fn merge_dir(source: &Path, target: &Path) -> Result<()> {
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        let source = entry.path();
        let target = target.join(entry.file_name());

        if source.is_dir() && target.is_dir() {
            merge_dir(&source, &target)?;
        } else if !target.exists() {
            fs::rename(&source, &target)?;
        } else {
            warn!(
                "Skipping merge because target already exists: {} -> {}",
                source.display(),
                target.display()
            );
        }
    }

    Ok(())
}
