//! LZH (LHA). Common in old Japanese BMS; names are Shift_JIS.

use super::{Extractor, decode_name, safe_join};
use anyhow::{Context, Result, bail};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

pub struct LzhExtractor;

impl Extractor for LzhExtractor {
    fn can_handle(&self, ext: &str) -> bool {
        ext == "lzh" || ext == "lha"
    }

    fn extract(&self, archive_path: &Path, target_dir: &Path) -> Result<Vec<PathBuf>> {
        let file = fs::File::open(archive_path).context("Failed to open archive")?;
        let mut reader = delharc::LhaDecodeReader::new(file).map_err(|e| anyhow::anyhow!("Failed to read lzh archive: {}", e))?;
        let mut out = Vec::new();
        loop {
            let header = reader.header();
            let name = header_name(header);
            let is_dir = header.is_directory();
            if let Some(path) = safe_join(target_dir, &name) {
                if is_dir {
                    fs::create_dir_all(&path)?;
                } else if reader.is_decoder_supported() {
                    if let Some(p) = path.parent() {
                        fs::create_dir_all(p)?;
                    }
                    let mut buf = Vec::new();
                    reader.read_to_end(&mut buf)?;
                    reader.crc_check().map_err(|e| anyhow::anyhow!("lzh CRC error in {}: {}", name, e))?;
                    fs::write(&path, buf)?;
                    out.push(path);
                } else {
                    bail!("Unsupported lzh method in {}", name);
                }
            }
            if !reader.next_file().map_err(|e| anyhow::anyhow!("Failed to read lzh archive: {}", e))? {
                break;
            }
        }
        Ok(out)
    }
}

/// The entry's path from the raw bytes (Shift_JIS), with the directory header if there is one.
fn header_name(h: &delharc::LhaHeader) -> String {
    let mut dir = String::new();
    let mut file = String::new();
    for extra in h.iter_extra() {
        match extra {
            [0x01, data @ ..] => file = decode_name(data),
            [0x02, data @ ..] => {
                let parts: Vec<String> = data.split(|&b| b == 0xff).filter(|p| !p.is_empty()).map(decode_name).collect();
                dir = parts.join("/");
            }
            _ => {}
        }
    }
    if file.is_empty() {
        file = decode_name(&h.filename).replace('\\', "/");
    }
    if dir.is_empty() { file } else { format!("{}/{}", dir, file) }
}
