use crate::client::RateLimitedClient;

use super::UrlParser;
use anyhow::{Context, Result};
use async_trait::async_trait;
use log::debug;
use regex::Regex;
use reqwest::{Url, header};
use scraper::{Html, Selector};
use std::sync::OnceLock;

pub struct GDriveParser;

#[async_trait]
impl UrlParser for GDriveParser {
    fn can_parse(&self, url: &str) -> bool {
        (url.contains("drive.google.com") || (url.contains("docs.google.com") && (url.contains("/uc") || url.contains("/file/d/"))))
            && !url.contains("drive.usercontent.google.com")
    }

    async fn parse(&self, client: &RateLimitedClient, url: &str) -> Result<Vec<String>> {
        // A shared folder: the files in it (the embedded view lists them without a login)
        if let Some(folder) = get_folder_id(url) {
            let view = format!("https://drive.google.com/embeddedfolderview?id={}", folder);
            let html = client.get(&view).await.send().await?.error_for_status()?.text().await?;
            static RE_FILE: OnceLock<Regex> = OnceLock::new();
            let re = RE_FILE.get_or_init(|| Regex::new(r"/(file/d|drive/folders)/([a-zA-Z0-9_-]{10,})").unwrap());
            let mut out: Vec<String> = Vec::new();
            for c in re.captures_iter(&html) {
                let id = &c[2];
                let u = if &c[1] == "file/d" {
                    format!("https://drive.google.com/file/d/{}/view", id)
                } else {
                    format!("https://drive.google.com/drive/folders/{}", id)
                };
                if id != folder && !out.contains(&u) {
                    out.push(u);
                }
            }
            debug!("[GDRIVE] folder {}: {} entries", folder, out.len());
            out.truncate(15);
            return Ok(out);
        }
        let file_id = get_drive_id(url).context("Failed to get Google Drive file ID")?;
        let download_url = format!(
            "https://drive.usercontent.google.com/download?id={}&export=download&authuser=0",
            file_id
        );

        debug!("[GDRIVE] ID: {}", file_id);
        debug!("[GDRIVE] URL: {}", download_url);

        let response = client
            .get(&download_url)
            .await
            .send()
            .await?
            .error_for_status()?;

        if let Some(content_type) = response.headers().get(header::CONTENT_TYPE) {
            if content_type.to_str().unwrap_or("").contains("text/html") {
                debug!("[GDRIVE] Virus scan warning page detected");
                let text = response.text().await?;

                let document = Html::parse_document(&text);
                let real_download_url = get_download_url_from_form(&document)
                    .context("Gogole Drive download link not found")?
                    .to_string();

                debug!("[GDRIVE] Parsing succeeded: {}", real_download_url);
                return Ok(vec![real_download_url]);
            }
        }

        debug!("[GDRIVE] Parsing succeeded: {}", download_url);
        Ok(vec![download_url])
    }
}

fn get_folder_id(url: &str) -> Option<String> {
    static RE: OnceLock<Regex> = OnceLock::new();
    let re = RE.get_or_init(|| Regex::new(r"/folders/([a-zA-Z0-9_-]+)|folderview\?id=([a-zA-Z0-9_-]+)").unwrap());
    let c = re.captures(url)?;
    Some(c.get(1).or(c.get(2))?.as_str().to_string())
}

/// The real download link on Google Drive's "can't scan for viruses" page, if this is that page.
pub fn form_url(html: &str) -> Option<String> {
    if !html.contains("download-form") {
        return None;
    }
    get_download_url_from_form(&Html::parse_document(html)).map(|u| u.to_string())
}

fn get_drive_id(url: &str) -> Option<String> {
    static RE_PATH: OnceLock<Regex> = OnceLock::new();
    let re_path = RE_PATH.get_or_init(|| Regex::new(r"/file/d/([a-zA-Z0-9_-]+)").unwrap());

    if let Some(caps) = re_path.captures(url) {
        return Some(caps.get(1)?.as_str().to_string());
    }

    static RE_QUERY: OnceLock<Regex> = OnceLock::new();
    let re_query = RE_QUERY.get_or_init(|| Regex::new(r"[?&]id=([a-zA-Z0-9_-]+)").unwrap());

    if let Some(caps) = re_query.captures(url) {
        return Some(caps.get(1)?.as_str().to_string());
    }

    None
}

fn get_download_url_from_form(document: &Html) -> Option<Url> {
    let form_selector = Selector::parse("form#download-form").ok()?;
    let input_selector = Selector::parse("input").ok()?;

    let form = document.select(&form_selector).next()?;
    let action = form.value().attr("action")?;

    let mut url = Url::parse(action).ok()?;

    for input in form.select(&input_selector) {
        if let (Some(name), Some(value)) = (input.value().attr("name"), input.value().attr("value"))
        {
            url.query_pairs_mut().append_pair(name, value);
        }
    }

    Some(url)
}
