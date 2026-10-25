//! Internet Archive (Wayback Machine). Many old BMS sites are gone but archived there.

use anyhow::Result;
use log::debug;
use reqwest::Url;

use crate::client::RateLimitedClient;

pub fn is_wayback(url: &str) -> bool {
    Url::parse(url).is_ok_and(|u| u.host_str() == Some("web.archive.org"))
}

/// The capture time in a Wayback URL (`/web/20010616011606/...` → "20010616011606").
pub fn timestamp(url: &Url) -> Option<String> {
    let mut seg = url.path_segments()?;
    if seg.next()? != "web" {
        return None;
    }
    let ts: String = seg.next()?.chars().take_while(|c| c.is_ascii_digit()).collect();
    (!ts.is_empty()).then_some(ts)
}

/// The archived file as it was (`id_`), not wrapped in the archive's toolbar page.
pub fn raw(url: &str) -> String {
    let Ok(u) = Url::parse(url) else { return url.to_string() };
    if u.host_str() != Some("web.archive.org") {
        return url.to_string();
    }
    let path = u.path();
    let Some(rest) = path.strip_prefix("/web/") else { return url.to_string() };
    let (ts, tail) = rest.split_once('/').unwrap_or((rest, ""));
    if ts.ends_with("id_") || !ts.chars().next().is_some_and(|c| c.is_ascii_digit()) {
        return url.to_string();
    }
    let ts: String = ts.chars().take_while(|c| c.is_ascii_digit()).collect();
    let mut out = format!("https://web.archive.org/web/{}id_/{}", ts, tail);
    if let Some(q) = u.query() {
        out.push('?');
        out.push_str(q);
    }
    out
}

#[derive(serde::Deserialize)]
struct Available {
    archived_snapshots: Snapshots,
}
#[derive(serde::Deserialize)]
struct Snapshots {
    closest: Option<Closest>,
}
#[derive(serde::Deserialize)]
struct Closest {
    available: bool,
    timestamp: String,
}

/// The archived copy of a dead URL, if the archive has one.
pub async fn lookup(client: &RateLimitedClient, url: &str) -> Result<Option<String>> {
    let mut api = Url::parse("https://archive.org/wayback/available")?;
    api.query_pairs_mut().append_pair("url", url);
    let res: Available = client.get(api).await.send().await?.error_for_status()?.json().await?;
    let found = res
        .archived_snapshots
        .closest
        .filter(|c| c.available)
        .map(|c| format!("https://web.archive.org/web/{}id_/{}", c.timestamp, url));
    debug!("[Wayback] {} -> {:?}", url, found);
    Ok(found)
}
