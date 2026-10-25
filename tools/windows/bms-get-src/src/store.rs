use anyhow::{Result, bail};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{BufReader, Read};
use std::path::Path;

pub fn save(source: &Path, output: &Path, name: &Path) -> Result<()> {
    let files = output.join("files");
    let packages = output.join("packages");
    fs::create_dir_all(&files)?;
    fs::create_dir_all(&packages)?;

    store_dir(source, &files)?;
    fs::rename(source, packages.join(name))?;

    Ok(())
}

fn store_dir(dir: &Path, files: &Path) -> Result<()> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        let file_type = entry.file_type()?;

        if file_type.is_dir() {
            store_dir(&path, files)?;
        } else if file_type.is_file() {
            store_file(&path, files)?;
        } else {
            bail!("Unsupported package entry: {}", path.display());
        }
    }

    Ok(())
}

fn store_file(path: &Path, files: &Path) -> Result<()> {
    let stored = files.join(sha256(path)?);

    // Hard links are not available on every file system (for example exFAT).
    // When they are not, keep the file where it is and skip the shared store.
    if stored.exists() {
        let tmp = path.with_extension("bms-get-link");
        if fs::hard_link(&stored, &tmp).is_ok() {
            fs::remove_file(path)?;
            fs::rename(&tmp, path)?;
        }
    } else {
        let _ = fs::hard_link(path, &stored);
    }
    Ok(())
}

fn sha256(path: &Path) -> Result<String> {
    let mut reader = BufReader::new(fs::File::open(path)?);
    let mut hasher = Sha256::new();
    let mut buffer = [0; 64 * 1024];

    loop {
        let read = reader.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }

    Ok(hex::encode(hasher.finalize()))
}
