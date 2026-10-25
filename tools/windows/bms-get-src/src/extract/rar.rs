use super::{Extractor, safe_join};
use anyhow::{Result, bail};
use std::fs;
use std::path::{Component, Path, PathBuf};
use unrar::Archive;

pub struct RarExtractor;

impl Extractor for RarExtractor {
    fn can_handle(&self, ext: &str) -> bool {
        ext == "rar"
    }

    fn extract(&self, archive_path: &Path, target_dir: &Path) -> Result<Vec<PathBuf>> {
        let mut archive = Archive::new(archive_path).open_for_processing()?;
        let mut extracted_paths = Vec::new();

        while let Some(header) = archive.read_header()? {
            let filename = &header.entry().filename;
            if filename.components().any(|component| {
                matches!(
                    component,
                    Component::ParentDir | Component::RootDir | Component::Prefix(_)
                )
            }) {
                bail!("Unsafe RAR entry path: {}", filename.display());
            }

            // Old archives keep names in Shift_JIS without saying so; the library reads them as CP437
            let name = fix_shift_jis(&filename.to_string_lossy());
            let Some(entry_path) = safe_join(target_dir, &name) else {
                archive = header.skip()?;
                continue;
            };
            archive = if header.entry().is_file() {
                if let Some(p) = entry_path.parent() {
                    fs::create_dir_all(p)?;
                }
                header.extract_to(&entry_path)?
            } else {
                fs::create_dir_all(&entry_path)?;
                header.skip()?
            };
            extracted_paths.push(entry_path);
        }

        Ok(extracted_paths)
    }
}

/// CP437's characters for the bytes 0x80..=0xFF.
const CP437_HIGH: &str = "ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥₧ƒáíóúñÑªº¿⌐¬½¼¡«»░▒▓│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀αßΓπΣσµτΦΘΩδ∞φε∩≡±≥≤⌠⌡÷≈°∙·√ⁿ²■\u{a0}";

/// A name that is Shift_JIS read as CP437 ("Æ╝" for "直"), turned back. Any other name is returned as it is.
pub fn fix_shift_jis(name: &str) -> String {
    if name.is_ascii() {
        return name.to_string();
    }
    let high: Vec<char> = CP437_HIGH.chars().collect();
    let mut bytes = Vec::with_capacity(name.len());
    for c in name.chars() {
        if c.is_ascii() {
            bytes.push(c as u8);
        } else if let Some(i) = high.iter().position(|&h| h == c) {
            bytes.push(0x80 + i as u8);
        } else {
            return name.to_string(); // a real non-ASCII name (already decoded correctly)
        }
    }
    let (decoded, _, had_errors) = encoding_rs::SHIFT_JIS.decode(&bytes);
    let japanese = decoded.chars().any(|c| matches!(c as u32, 0x3040..=0x30ff | 0x4e00..=0x9fff | 0xff61..=0xff9f));
    if had_errors || !japanese { name.to_string() } else { decoded.into_owned() }
}

#[cfg(test)]
mod tests {
    use super::fix_shift_jis;

    #[test]
    fn shift_jis_read_as_cp437() {
        assert_eq!(fix_shift_jis("Æ╝ -ST.- [ogg]"), "直 -ST.- [ogg]");
        assert_eq!(fix_shift_jis("kick.wav"), "kick.wav");
        assert_eq!(fix_shift_jis("直.wav"), "直.wav");
        assert_eq!(fix_shift_jis("café.wav"), "café.wav");
    }
}
