use anyhow::{Context, Result};
use reqwest::Url;
use scraper::{Html, Selector};

use crate::client::RateLimitedClient;

#[derive(serde::Deserialize)]
struct Header {
    name: String,
    symbol: String,
    data_url: String,
}

#[derive(serde::Deserialize)]
pub struct BmsData {
    pub md5: String,
    pub level: String,
    pub title: Option<String>,
    pub artist: Option<String>,
    #[serde(rename = "url")]
    pub main_url: Option<String>,
    #[serde(rename = "url_diff")]
    pub diff_url: Option<String>,
}

pub struct Table {
    pub name: String,
    pub symbol: String,
    pub bms_data: Vec<BmsData>,
}

pub async fn parse_table(client: &RateLimitedClient, table_url: &str) -> Result<Table> {
    let response = client.get(table_url).await.send().await?;
    let text = response.text().await?;

    let document = Html::parse_document(&text);
    let selector = Selector::parse("head > meta[name='bmstable']").unwrap();

    let header_path = document
        .select(&selector)
        .next()
        .and_then(|el| el.value().attr("content"))
        .context("bmstable not found")?;
    let header_url = Url::parse(table_url)?.join(header_path)?;

    let response = client.get(header_url.clone()).await.send().await?;
    let header: Header = response.json().await?;

    let name = header.name;
    let symbol = header.symbol;

    let data_url = header_url.join(&header.data_url)?;

    let response = client.get(data_url).await.send().await?;
    let bms_data: Vec<BmsData> = response.json().await?;

    Ok(Table {
        name,
        symbol,
        bms_data,
    })
}
