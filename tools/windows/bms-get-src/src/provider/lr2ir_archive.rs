use anyhow::{Context, Result};
use async_trait::async_trait;
use log::debug;
use reqwest::StatusCode;

use crate::client::RateLimitedClient;

use super::{BmsProvider, BmsUrl};

pub struct Lr2IrArchiveProvider;

#[derive(serde::Deserialize)]
struct ChartResponse {
    chart: Chart,
}

#[derive(serde::Deserialize)]
struct Chart {
    body_url: Option<String>,
    diff_url: Option<String>,
}

#[async_trait]
impl BmsProvider for Lr2IrArchiveProvider {
    fn name(&self) -> &'static str {
        "LR2IR Archive"
    }

    async fn find_urls(&self, client: &RateLimitedClient, md5: &str) -> Result<BmsUrl> {
        let api_url = format!("https://lr2ir.com/api/charts/{}", md5);
        debug!("[LR2IR Archive] URL: {}", api_url);

        let response = client.get(&api_url).await.send().await?;
        if response.status() == StatusCode::NOT_FOUND {
            return Ok(BmsUrl::default());
        }

        let response = response.error_for_status()?;
        let chart: ChartResponse = response
            .json()
            .await
            .context("Parsing LR2IR Archive response failed")?;

        Ok(BmsUrl {
            main_urls: chart.chart.body_url.map_or(vec![], |url| vec![url]),
            diff_urls: chart.chart.diff_url.map_or(vec![], |url| vec![url]),
        })
    }
}
