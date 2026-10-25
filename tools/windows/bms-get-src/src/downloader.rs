use anyhow::{Context, Result, bail};
use futures_util::StreamExt;
use log::debug;
use percent_encoding::percent_decode_str;
use regex::Regex;
use reqwest::{Response, Url, header};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;
use tokio::fs::File;
use tokio::io::AsyncWriteExt;

use crate::client::RateLimitedClient;
use crate::sniff::{self, Kind};
use crate::{extract, page, site};

/// Pages are read up to this size (a bigger "page" is not one).
const MAX_PAGE: usize = 8 << 20;

/// What a URL gave: files to extract, or a page whose links may lead to the file.
pub enum Fetched {
    Files(Vec<PathBuf>),
    Page(Vec<String>),
    /// A "prove you are human" page: load it again with this cookie
    Retry(String),
}

pub async fn download(
    client: &RateLimitedClient,
    url: &str,
    output_dir: &Path,
    hint: Option<&str>,
    max_bytes: u64,
    on_progress: Box<dyn Fn(u64, u64) + Send + Sync>,
) -> Result<Fetched> {
    debug!("[Downloader] ダウンロード開始: {}", url);

    // A file kept on this PC (a mirror taken in advance): file:///E:/...
    if let Some(local) = local_path(url) {
        let name = local.file_name().context("file: URL without a name")?;
        let dst = output_dir.join(name);
        tokio::fs::copy(&local, &dst).await.with_context(|| format!("copy {}", local.display()))?;
        return Ok(Fetched::Files(vec![dst]));
    }

    if site::mega::matches(url) {
        return Ok(Fetched::Files(site::mega::download(client, url, output_dir).await?));
    }
    if site::onedrive::matches(url) {
        let mut out = Vec::new();
        for (name, direct) in site::onedrive::files(client, url).await? {
            let res = client.get(&direct).await.send().await?.error_for_status()?;
            if let Fetched::Files(p) = save(res, &direct, Some(name), output_dir, hint, max_bytes, &on_progress).await? {
                out.extend(p);
            }
        }
        if out.is_empty() {
            bail!("OneDrive: no file");
        }
        return Ok(Fetched::Files(out));
    }

    let url = site::wayback::raw(url);
    // Sites that moved (the old address only redirects with JavaScript)
    let url = url.replace("://darksabun.github.io/", "://darksabun.club/");
    let url = if site::mediafire::matches(&url) { site::mediafire::direct_url(client, &url).await? } else { url };
    let response = if site::getuploader::matches(&url) {
        site::getuploader::fetch(client, &url).await?
    } else {
        client
            .get(&url)
            .await
            .header(header::REFERER, referer(&url))
            .send()
            .await?
            .error_for_status()?
    };
    match save(response, &url, None, output_dir, hint, max_bytes, &on_progress).await? {
        Fetched::Retry(cookie) => {
            debug!("[Downloader] human check passed with {}", cookie);
            let again = client
                .get(&url)
                .await
                .header(header::REFERER, referer(&url))
                .header(header::COOKIE, cookie)
                .send()
                .await?
                .error_for_status()?;
            match save(again, &url, None, output_dir, hint, max_bytes, &on_progress).await? {
                Fetched::Retry(_) => bail!("Could not pass the human check: {}", url),
                f => Ok(f),
            }
        }
        f => Ok(f),
    }
}

/// The site's top page as the referer (some old hosts refuse direct links without one).
fn referer(url: &str) -> String {
    Url::parse(url).map(|u| format!("{}://{}/", u.scheme(), u.host_str().unwrap_or(""))).unwrap_or_default()
}

async fn save(
    response: Response,
    url: &str,
    name: Option<String>,
    output_dir: &Path,
    hint: Option<&str>,
    max_bytes: u64,
    on_progress: &(dyn Fn(u64, u64) + Send + Sync),
) -> Result<Fetched> {
    let final_url = response.url().clone();
    let content_type = response.headers().get(header::CONTENT_TYPE).and_then(|v| v.to_str().ok()).map(str::to_string);
    let header_name = response.headers().get(header::CONTENT_DISPOSITION).and_then(|v| get_filename_from_header(v.as_bytes()));
    let total_size = response.content_length().unwrap_or(0);
    let mut stream = response.bytes_stream();

    // Look at the first bytes to know what this is
    let mut head = Vec::new();
    while head.len() < 512 {
        match stream.next().await {
            Some(chunk) => head.extend_from_slice(&chunk?),
            None => break,
        }
    }
    let kind = sniff::kind(&head);
    let is_bmson = header_name.as_deref().unwrap_or(final_url.path()).to_lowercase().ends_with(".bmson");
    if kind == Kind::Json && !is_bmson {
        let mut body = head;
        while let Some(chunk) = stream.next().await {
            body.extend_from_slice(&chunk?);
            if body.len() > MAX_PAGE {
                bail!("Page too large: {}", url);
            }
        }
        let links = page::json_links(&final_url, &String::from_utf8_lossy(&body), hint);
        debug!("[Downloader] json {} → {} links", final_url, links.len());
        return Ok(Fetched::Page(links));
    }
    let looks_html = kind == Kind::Html
        || (kind == Kind::Other && content_type.as_deref().is_some_and(|t| t.contains("text/html")) && header_name.is_none());
    if looks_html {
        let mut body = head;
        while let Some(chunk) = stream.next().await {
            body.extend_from_slice(&chunk?);
            if body.len() > MAX_PAGE {
                bail!("Page too large: {}", url);
            }
        }
        let html = page::decode(&body, content_type.as_deref());
        if let Some(cookie) = site::testcookie::solve(&html) {
            return Ok(Fetched::Retry(cookie));
        }
        // Google Drive's "can't scan this file for viruses" page: its form is the real link
        if let Some(u) = crate::parser::gdrive::form_url(&html) {
            return Ok(Fetched::Page(vec![u]));
        }
        let links = page::links(&final_url, &html, hint, max_bytes == u64::MAX);
        debug!("[Downloader] page {} → {} links", final_url, links.len());
        for l in &links {
            debug!("[Downloader]   {}", l);
        }
        return Ok(Fetched::Page(links));
    }

    let mut filename = name
        .or(header_name)
        .or_else(|| get_filename_from_url(final_url.as_str()))
        .or_else(|| get_filename_from_url(url))
        .unwrap_or_else(|| "download".to_string());
    // The name says nothing (or the wrong thing) about the content: name it by what it is
    let named_ok = extract::find_extractor(Path::new(&filename)).is_ok();
    match kind.ext() {
        Some(ext) if !filename.to_lowercase().ends_with(&format!(".{}", ext)) && (!named_ok || kind != Kind::Chart) => {
            filename = format!("{}.{}", filename, ext);
        }
        None if !named_ok => bail!("Not an archive or a chart: {} ({})", filename, content_type.unwrap_or_default()),
        _ => {}
    }
    debug!("[Downloader] ファイル名: {}", filename);

    // A whole event's pack found on some page is rarely the one song wanted
    if total_size > max_bytes {
        bail!("Too large ({} MB): {}", total_size >> 20, filename);
    }
    let save_path = output_dir.join(&filename);
    extract::find_extractor(&save_path).context("No extractor found")?;
    tokio::fs::create_dir_all(output_dir).await?;
    let mut file = File::create(&save_path).await?;
    file.write_all(&head).await?;
    on_progress(head.len() as u64, total_size);
    let mut written = head.len() as u64;
    while let Some(item) = stream.next().await {
        let chunk = item?;
        written += chunk.len() as u64;
        if written > max_bytes {
            drop(file);
            let _ = tokio::fs::remove_file(&save_path).await;
            bail!("Too large (over {} MB): {}", max_bytes >> 20, filename);
        }
        file.write_all(&chunk).await?;
        on_progress(chunk.len() as u64, total_size);
    }
    file.flush().await?;
    Ok(Fetched::Files(vec![save_path]))
}

fn get_filename_from_url(url: &str) -> Option<String> {
    let url = url.split(['?', '#']).next()?;
    let last = url.trim_end_matches('/').rsplit('/').next()?;
    let last = percent_decode_str(last).decode_utf8_lossy().into_owned();
    // MediaFire style: /file/<id>/<name>/file
    if last == "file" {
        return url.trim_end_matches("/file").rsplit('/').next().map(|s| sanitize(&percent_decode_str(s).decode_utf8_lossy()));
    }
    if last.contains('.') { Some(sanitize(&last)) } else { None }
}

/// `filename*=UTF-8''...` (RFC 5987), then `filename="..."` (UTF-8, Shift_JIS, or percent-encoded).
fn get_filename_from_header(raw: &[u8]) -> Option<String> {
    let text = match std::str::from_utf8(raw) {
        Ok(s) => s.to_string(),
        Err(_) => encoding_rs::SHIFT_JIS.decode(raw).0.into_owned(),
    };
    static RE_EXT: OnceLock<Regex> = OnceLock::new();
    let re_ext = RE_EXT.get_or_init(|| Regex::new(r#"(?i)filename\*\s*=\s*([^']*)'[^']*'([^;]+)"#).unwrap());
    if let Some(c) = re_ext.captures(&text) {
        let v = percent_decode_str(c.get(2)?.as_str().trim().trim_matches('"')).decode_utf8_lossy().into_owned();
        return Some(sanitize(&v));
    }
    static RE: OnceLock<Regex> = OnceLock::new();
    let re = RE.get_or_init(|| Regex::new(r#"(?i)filename\s*=\s*"?([^";]+)"?"#).unwrap());
    let v = re.captures(&text)?.get(1)?.as_str().trim().to_string();
    let v = if v.contains('%') { percent_decode_str(&v).decode_utf8_lossy().into_owned() } else { v };
    Some(sanitize(&v))
}

pub fn sanitize(name: &str) -> String {
    let s: String = name
        .chars()
        .map(|c| {
            if c.is_control() || ['/', '\\', ':', '*', '?', '"', '<', '>', '|'].contains(&c) {
                '_'
            } else {
                c
            }
        })
        .collect();

    let s = s.trim().trim_end_matches('.');

    if s.is_empty() {
        return "downloaded_file".to_string();
    }

    s.to_string()
}

fn local_path(url: &str) -> Option<PathBuf> {
    let rest = url.strip_prefix("file:///")?;
    let path = percent_decode_str(rest).decode_utf8_lossy().replace('/', std::path::MAIN_SEPARATOR_STR);
    // file:///E:/a.zip on Windows, file:///home/a.zip elsewhere
    Some(PathBuf::from(if path.as_bytes().get(1) == Some(&b':') { path } else { format!("{}{}", std::path::MAIN_SEPARATOR, path) }))
}
