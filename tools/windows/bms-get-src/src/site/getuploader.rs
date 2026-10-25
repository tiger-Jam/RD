//! getuploader (uploader.jp): the download page asks to press "ダウンロード" (a form with a token);
//! the page after that has the real link. Uploads with a password cannot be taken.

use anyhow::{Context, Result, bail};
use reqwest::{Response, Url};
use scraper::{Html, Selector};

use crate::client::RateLimitedClient;

pub fn matches(url: &str) -> bool {
    Url::parse(url).is_ok_and(|u| u.host_str().is_some_and(|h| h.ends_with("getuploader.com")) && u.path().contains("/download/"))
}

pub async fn fetch(client: &RateLimitedClient, url: &str) -> Result<Response> {
    let html = client.get(url).await.send().await?.error_for_status()?.text().await?;
    let token = {
        let doc = Html::parse_document(&html);
        if doc.select(&Selector::parse("input[type=password]").unwrap()).next().is_some() {
            bail!("getuploader: needs a password");
        }
        let sel = Selector::parse("input[name=token]").unwrap();
        doc.select(&sel)
            .next()
            .and_then(|e| e.value().attr("value"))
            .map(str::to_string)
            .context("getuploader: no download form (the file may be gone)")?
    };
    let res = client
        .post(url)
        .await
        .header(reqwest::header::REFERER, url)
        .form(&[("token", token.as_str()), ("yes", "ダウンロード")])
        .send()
        .await?
        .error_for_status()?;
    Ok(res)
}
