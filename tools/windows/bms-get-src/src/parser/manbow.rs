use anyhow::Result;
use anyhow::anyhow;
use async_trait::async_trait;
use encoding_rs::SHIFT_JIS;
use scraper::{Html, Selector};

use crate::{client::RateLimitedClient, parser::UrlParser};

pub struct ManbowParser;

#[async_trait]
impl UrlParser for ManbowParser {
    fn can_parse(&self, url: &str) -> bool {
        url.contains("manbow.nothing.sh")
    }

    async fn parse(&self, client: &RateLimitedClient, url: &str) -> Result<Vec<String>> {
        let bytes = client.get(url).await.send().await?.bytes().await?;
        let (decoded, _, _) = SHIFT_JIS.decode(&bytes);
        let response = decoded.into_owned();

        let document = Html::parse_document(&response);

        let tr_selector = Selector::parse("tr").unwrap();
        let a_selector = Selector::parse("a").unwrap();

        for tr in document.select(&tr_selector) {
            let tr_text = tr.text().collect::<Vec<_>>().join("");

            if tr_text.contains("DownLoadAddress") {
                let mut urls: Vec<String> = vec![];

                for a in tr.select(&a_selector) {
                    if let Some(href) = a.value().attr("href") {
                        urls.push(href.to_string());
                    }
                }

                return Ok(urls);
            }
        }

        Err(anyhow!("No manbow parser found"))
    }
}
