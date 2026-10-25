use std::collections::HashSet;

use anyhow::{Context, Result};
use async_trait::async_trait;
use encoding_rs::SHIFT_JIS;
use reqwest::Url;
use scraper::{Html, Selector};

use crate::client::RateLimitedClient;

use super::{EventEntry, EventScraper};

pub struct ManbowEventScraper;

#[async_trait]
impl EventScraper for ManbowEventScraper {
    fn can_handle(&self, url: &str) -> bool {
        let Ok(url) = Url::parse(url) else {
            return false;
        };

        url.host_str() == Some("manbow.nothing.sh")
            && url.path() == "/event/event.cgi"
            && event_id(&url).is_some()
    }

    async fn scrape(&self, client: &RateLimitedClient, url: &str) -> Result<Vec<EventEntry>> {
        let url = Url::parse(url)?;
        let url_list_url = url_list_url(&url)?;

        let bytes = client
            .get(url_list_url.clone())
            .await
            .send()
            .await?
            .bytes()
            .await?;
        let (decoded, _, _) = SHIFT_JIS.decode(&bytes);
        let document = Html::parse_document(&decoded);

        parse_url_list(&document, &url_list_url)
    }
}

fn event_id(url: &Url) -> Option<String> {
    url.query_pairs()
        .find(|(key, _)| key == "event")
        .map(|(_, value)| value.into_owned())
}

fn url_list_url(url: &Url) -> Result<Url> {
    let event = event_id(url).context("Manbow event ID not found")?;
    let mut url = url.clone();
    url.set_path("/event/event.cgi");
    url.query_pairs_mut()
        .clear()
        .append_pair("action", "URLList")
        .append_pair("event", &event)
        .append_pair("end", "999");
    Ok(url)
}

fn parse_url_list(document: &Html, base_url: &Url) -> Result<Vec<EventEntry>> {
    let table = find_url_list_table(document).context("Manbow URL list table not found")?;
    let tr_selector = Selector::parse("tr").unwrap();
    let td_selector = Selector::parse("td").unwrap();
    let a_selector = Selector::parse("a").unwrap();
    let mut entries = Vec::new();

    for tr in table.select(&tr_selector) {
        let cells: Vec<_> = tr.select(&td_selector).collect();
        let Some(url_cell) = cells.last() else {
            continue;
        };

        let mut urls = Vec::new();
        let mut seen = HashSet::new();

        for a in url_cell.select(&a_selector) {
            let Some(href) = a.value().attr("href") else {
                continue;
            };
            let Ok(url) = base_url.join(href) else {
                continue;
            };
            if !matches!(url.scheme(), "http" | "https") {
                continue;
            }

            let url = url.to_string();
            if seen.insert(url.clone()) {
                urls.push(url);
            }
        }

        if !urls.is_empty() {
            entries.push(EventEntry { urls });
        }
    }

    Ok(entries)
}

fn find_url_list_table<'a>(document: &'a Html) -> Option<scraper::ElementRef<'a>> {
    let list_selector = Selector::parse("table#list").unwrap();
    let dllist_selector = Selector::parse("table#dllist").unwrap();

    document
        .select(&list_selector)
        .next()
        .or_else(|| document.select(&dllist_selector).next())
}
