use anyhow::{Context, Result};
use async_trait::async_trait;

use crate::client::RateLimitedClient;

use super::{BmsProvider, BmsUrl};

pub struct BmsSearchProvider;

#[derive(serde::Deserialize)]
struct BmsPattern {
    bms: BmsId,
}

#[derive(serde::Deserialize)]
struct BmsId {
    id: Option<String>,
}

#[derive(serde::Deserialize)]
struct BmsData {
    downloads: Option<Vec<DownloadUrl>>,
}

#[derive(serde::Deserialize)]
struct DownloadUrl {
    url: Option<String>,
}

#[async_trait]
impl BmsProvider for BmsSearchProvider {
    fn name(&self) -> &'static str {
        "BMS SEARCH API"
    }

    async fn find_urls(&self, client: &RateLimitedClient, md5: &str) -> Result<BmsUrl> {
        let api_url = format!("https://api.bmssearch.net/v1/patterns/{}", md5);
        let response = client.get(&api_url).await.send().await?;

        let pattern: BmsPattern = response.json().await.context("Parsing failed")?;
        let id = pattern.bms.id.context("BMS ID not found")?;

        let api_url = format!("https://api.bmssearch.net/v1/bmses/{}", id);
        let response = client.get(&api_url).await.send().await?;

        let bms: BmsData = response.json().await.context("Parsing failed")?;
        let urls: Vec<String> = bms
            .downloads
            .context("URL not found")?
            .iter()
            .filter_map(|u| u.url.clone())
            .collect();

        Ok(BmsUrl {
            main_urls: urls,
            diff_urls: vec![],
        })
    }
}
