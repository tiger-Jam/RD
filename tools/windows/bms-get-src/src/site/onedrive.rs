//! OneDrive (personal). Shared links are opened through the same API the web page uses, with its anonymous token.
//! Old links (`onedrive.live.com/?cid=..&id=..`) are turned into `redir?resid=..` first.

use anyhow::{Context, Result, bail};
use base64::Engine;
use reqwest::Url;

use crate::client::RateLimitedClient;

pub fn matches(url: &str) -> bool {
    Url::parse(url).is_ok_and(|u| {
        u.host_str().is_some_and(|h| matches!(h, "1drv.ms" | "onedrive.live.com" | "skydrive.live.com" | "sdrv.ms"))
    })
}

#[derive(serde::Deserialize)]
struct Token {
    token: String,
}

#[derive(serde::Deserialize)]
struct Item {
    name: Option<String>,
    #[serde(rename = "@content.downloadUrl")]
    download_url: Option<String>,
    folder: Option<serde_json::Value>,
    children: Option<Vec<Item>>,
}

fn share_url(url: &str) -> String {
    let Ok(u) = Url::parse(url) else { return url.to_string() };
    let q: std::collections::HashMap<_, _> = u.query_pairs().into_owned().collect();
    if let Some(id) = q.get("id").or(q.get("resid")) {
        let mut r = Url::parse("https://onedrive.live.com/redir").unwrap();
        r.query_pairs_mut().append_pair("resid", id);
        if let Some(k) = q.get("authkey") {
            r.query_pairs_mut().append_pair("authkey", k);
        }
        return r.to_string();
    }
    url.to_string()
}

/// The files behind the link (a folder gives each file in it): (name, direct URL).
pub async fn files(client: &RateLimitedClient, url: &str) -> Result<Vec<(String, String)>> {
    let token: Token = client
        .post("https://api-badgerp.svc.ms/v1.0/token")
        .await
        .json(&serde_json::json!({"appId": "5cbed6ac-a083-4e14-b191-b4ba07653de2"}))
        .send()
        .await?
        .error_for_status()?
        .json()
        .await?;
    let encoded = base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(share_url(url));
    let api = format!("https://my.microsoftpersonalcontent.com/_api/v2.0/shares/u!{}/driveitem", encoded);
    let get = |u: String| async {
        let res = client
            .get(u)
            .await
            .header("Authorization", format!("Badger {}", token.token))
            .header("Prefer", "autoredeem")
            .send()
            .await?;
        if !res.status().is_success() {
            bail!("OneDrive: {} (the link may be gone)", res.status());
        }
        Ok::<Item, anyhow::Error>(res.json().await?)
    };
    let item = get(api.clone()).await?;
    if item.folder.is_none() {
        let name = item.name.unwrap_or_else(|| "download".into());
        return Ok(vec![(name, item.download_url.context("OneDrive: no download URL")?)]);
    }
    let folder = get(format!("{}?$expand=children", api)).await?;
    Ok(folder
        .children
        .unwrap_or_default()
        .into_iter()
        .filter_map(|c| Some((c.name?, c.download_url?)))
        .collect())
}
