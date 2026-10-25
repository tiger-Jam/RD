//! MediaFire: the file page has the real link on its download button.

use anyhow::{Context, Result};
use reqwest::Url;
use scraper::{Html, Selector};

use crate::client::RateLimitedClient;

pub fn matches(url: &str) -> bool {
    Url::parse(url).is_ok_and(|u| {
        u.host_str().is_some_and(|h| h == "mediafire.com" || h == "www.mediafire.com")
            && (u.path().starts_with("/file/") || u.path().starts_with("/download/") || u.query().is_some())
    })
}

pub async fn direct_url(client: &RateLimitedClient, url: &str) -> Result<String> {
    let html = client.get(url).await.send().await?.error_for_status()?.text().await?;
    let doc = Html::parse_document(&html);
    let sel = Selector::parse("a#downloadButton").unwrap();
    let a = doc.select(&sel).next().context("MediaFire: no download button (the file may be gone)")?;
    let href = a.value().attr("href").filter(|h| h.starts_with("http"));
    // Newer pages keep the link base64-encoded in data-scrambled-url
    let href = match href {
        Some(h) => h.to_string(),
        None => {
            use base64::Engine;
            let s = a.value().attr("data-scrambled-url").context("MediaFire: no link on the button")?;
            String::from_utf8(base64::engine::general_purpose::STANDARD.decode(s)?)?
        }
    };
    Ok(href)
}
