//! MEGA. Files are encrypted (AES-128-CTR) with the key in the link; the name is in the encrypted attributes.
//! Folder links list their files (each file key is encrypted with the folder key).

use aes::Aes128;
use aes::cipher::{BlockCipherDecrypt, KeyInit, KeyIvInit, StreamCipher};
use anyhow::{Context, Result, bail};
use base64::Engine;
use futures_util::StreamExt;
use log::{debug, info};
use std::path::{Path, PathBuf};
use tokio::io::AsyncWriteExt;

use crate::client::RateLimitedClient;

const API: &str = "https://g.api.mega.co.nz/cs";
/// Files taken from one folder link, at most (a folder can hold a whole uploader's works).
const MAX_FOLDER_FILES: usize = 8;

pub fn matches(url: &str) -> bool {
    url.contains("mega.nz/") || url.contains("mega.co.nz/")
}

enum Link {
    File { id: String, key: Vec<u8> },
    Folder { id: String, key: Vec<u8>, only: Option<String> },
}

fn b64(s: &str) -> Result<Vec<u8>> {
    let s: String = s.chars().filter(|c| !c.is_whitespace()).collect();
    Ok(base64::engine::general_purpose::URL_SAFE_NO_PAD.decode(s.trim_end_matches('=').replace('+', "-").replace('/', "_"))?)
}

fn parse(url: &str) -> Result<Link> {
    // #!id!key, #F!id!key[!sub|?sub], /file/id#key, /folder/id#key[/file/sub]
    let (path, frag) = url.split_once('#').map(|(a, b)| (a, b.to_string())).unwrap_or((url, String::new()));
    if let Some(rest) = frag.strip_prefix('!') {
        let (id, key) = rest.split_once('!').context("MEGA: bad link")?;
        return Ok(Link::File { id: id.into(), key: b64(key)? });
    }
    if let Some(rest) = frag.strip_prefix("F!") {
        let mut it = rest.splitn(2, '!');
        let id = it.next().context("MEGA: bad link")?;
        let rest = it.next().context("MEGA: bad link")?;
        let (key, only) = match rest.find(['!', '?']) {
            Some(i) => (&rest[..i], Some(rest[i + 1..].to_string())),
            None => (rest, None),
        };
        return Ok(Link::Folder { id: id.into(), key: b64(key)?, only });
    }
    let after = |tag: &str| path.split(tag).nth(1).map(|s| s.trim_end_matches('/').to_string());
    if let Some(id) = after("/file/") {
        return Ok(Link::File { id, key: b64(&frag)? });
    }
    if let Some(id) = after("/folder/") {
        let (key, only) = match frag.split_once("/file/") {
            Some((k, s)) => (k.to_string(), Some(s.to_string())),
            None => (frag.split('/').next().unwrap_or("").to_string(), None),
        };
        return Ok(Link::Folder { id, key: b64(&key)?, only });
    }
    bail!("MEGA: unknown link form")
}

fn words(b: &[u8]) -> Vec<u32> {
    b.chunks(4).map(|c| u32::from_be_bytes([c[0], c[1], c.get(2).copied().unwrap_or(0), c.get(3).copied().unwrap_or(0)])).collect()
}

/// A 32-byte file key → AES key and CTR IV.
fn file_key(k: &[u8]) -> Result<([u8; 16], [u8; 16])> {
    if k.len() < 32 {
        bail!("MEGA: short key");
    }
    let a = words(k);
    let mut key = [0u8; 16];
    for i in 0..4 {
        key[i * 4..i * 4 + 4].copy_from_slice(&(a[i] ^ a[i + 4]).to_be_bytes());
    }
    let mut iv = [0u8; 16];
    iv[..8].copy_from_slice(&k[16..24]);
    Ok((key, iv))
}

fn ecb_decrypt(key: &[u8; 16], data: &mut [u8]) {
    let c = Aes128::new(key.into());
    for block in data.chunks_mut(16) {
        if block.len() == 16 {
            c.decrypt_block(block.try_into().unwrap());
        }
    }
}

/// The name in the encrypted attributes ("MEGA{"n":"name"}", AES-CBC with a zero IV).
fn attr_name(key: &[u8; 16], at: &str) -> Option<String> {
    let mut data = b64(at).ok()?;
    let c = Aes128::new(key.into());
    let mut prev = [0u8; 16];
    for block in data.chunks_mut(16) {
        if block.len() < 16 {
            break;
        }
        let cur: [u8; 16] = block.try_into().ok()?;
        c.decrypt_block(block.try_into().unwrap());
        for (b, p) in block.iter_mut().zip(prev.iter()) {
            *b ^= p;
        }
        prev = cur;
    }
    let s = String::from_utf8_lossy(&data);
    let s = s.strip_prefix("MEGA")?.trim_end_matches('\0');
    let v: serde_json::Value = serde_json::from_str(s).ok()?;
    v.get("n")?.as_str().map(str::to_string)
}

async fn api(client: &RateLimitedClient, folder: Option<&str>, body: serde_json::Value) -> Result<serde_json::Value> {
    let mut url = format!("{}?id={}", API, rand_id());
    if let Some(f) = folder {
        url.push_str("&n=");
        url.push_str(f);
    }
    let v: serde_json::Value = client.post(url).await.json(&serde_json::json!([body])).send().await?.error_for_status()?.json().await?;
    let first = if v.is_array() { v[0].clone() } else { v };
    if let Some(code) = first.as_i64() {
        bail!("MEGA: error {} (the file may be gone)", code);
    }
    Ok(first)
}

fn rand_id() -> u32 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.subsec_nanos()).unwrap_or(1)
}

pub async fn download(client: &RateLimitedClient, url: &str, out_dir: &Path) -> Result<Vec<PathBuf>> {
    match parse(url)? {
        Link::File { id, key } => {
            let (k, iv) = file_key(&key)?;
            let info = api(client, None, serde_json::json!({"a": "g", "g": 1, "p": id, "ssl": 2})).await?;
            Ok(vec![fetch(client, &info, &k, &iv, out_dir).await?])
        }
        Link::Folder { id, key, only } => {
            let fkey: [u8; 16] = key.get(..16).context("MEGA: short folder key")?.try_into()?;
            let list = api(client, Some(&id), serde_json::json!({"a": "f", "c": 1, "r": 1, "ca": 1})).await?;
            let mut out = Vec::new();
            debug!("[MEGA] folder {}: {} nodes, only={:?}", id, list["f"].as_array().map_or(0, |a| a.len()), only);
            for node in list["f"].as_array().cloned().unwrap_or_default() {
                let h = node["h"].as_str().unwrap_or("").to_string();
                if node["t"].as_i64() != Some(0) {
                    continue;
                }
                if only.as_ref().is_some_and(|o| *o != h) {
                    continue;
                }
                // "owner:key/owner:key": the one encrypted with this folder's key decrypts the attributes
                let at = node["a"].as_str().unwrap_or("");
                let mut found = None;
                for part in node["k"].as_str().unwrap_or("").split('/') {
                    let Some(enc) = part.split(':').nth(1) else { continue };
                    let Ok(mut nk) = b64(enc) else { continue };
                    ecb_decrypt(&fkey, &mut nk);
                    let Ok((k, iv)) = file_key(&nk) else { continue };
                    if let Some(name) = attr_name(&k, at) {
                        found = Some((k, iv, name));
                        break;
                    }
                }
                debug!("[MEGA] node {} t={} decrypted={:?}", h, node["t"], found.as_ref().map(|f| &f.2));
                let Some((k, iv, name)) = found else { continue };
                let lower = name.to_lowercase();
                if only.is_none()
                    && !["zip", "rar", "7z", "lzh", "lha", "exe", "bms", "bme", "bml", "pms", "bmson"].iter().any(|e| lower.ends_with(&format!(".{}", e)))
                {
                    continue;
                }
                let info = api(client, Some(&id), serde_json::json!({"a": "g", "g": 1, "n": h, "ssl": 2})).await?;
                let mut info = info;
                info["at"] = node["a"].clone();
                match fetch(client, &info, &k, &iv, out_dir).await {
                    Ok(p) => out.push(p),
                    Err(e) => debug!("[MEGA] {}: {}", name, e),
                }
                if out.len() >= MAX_FOLDER_FILES {
                    break;
                }
            }
            if out.is_empty() {
                bail!("MEGA: no archive in the folder");
            }
            Ok(out)
        }
    }
}

async fn fetch(client: &RateLimitedClient, info: &serde_json::Value, key: &[u8; 16], iv: &[u8; 16], out_dir: &Path) -> Result<PathBuf> {
    let g = info["g"].as_str().context("MEGA: no download URL (over quota?)")?;
    let name = info["at"].as_str().and_then(|a| attr_name(key, a)).unwrap_or_else(|| "mega_download".into());
    let name = crate::downloader::sanitize(&name);
    info!("[MEGA] {}", name);
    let res = client.get(g).await.send().await?.error_for_status()?;
    tokio::fs::create_dir_all(out_dir).await?;
    let path = out_dir.join(&name);
    let mut file = tokio::fs::File::create(&path).await?;
    let mut cipher = ctr::Ctr128BE::<Aes128>::new(key.into(), iv.into());
    let mut stream = res.bytes_stream();
    while let Some(chunk) = stream.next().await {
        let mut chunk = chunk?.to_vec();
        cipher.apply_keystream(&mut chunk);
        file.write_all(&chunk).await?;
    }
    file.flush().await?;
    Ok(path)
}
