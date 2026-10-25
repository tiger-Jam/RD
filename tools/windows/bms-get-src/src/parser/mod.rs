use anyhow::Result;
use async_trait::async_trait;

use crate::client::RateLimitedClient;

pub mod dropbox;
pub mod gdrive;
pub mod manbow;

#[derive(Debug)]
pub enum ParseResult {
    Links(Vec<String>),
    File(String),
}

#[async_trait]
pub trait UrlParser: Send + Sync {
    fn can_parse(&self, url: &str) -> bool;
    async fn parse(&self, client: &RateLimitedClient, url: &str) -> Result<Vec<String>>;
}

const PARSERS: &[&dyn UrlParser] = &[
    &gdrive::GDriveParser,
    &dropbox::DropboxParser,
    &manbow::ManbowParser,
    // 新規パーサーはここに追加
];

pub async fn parse_url(client: &RateLimitedClient, url: &str) -> Result<ParseResult> {
    for parser in PARSERS {
        if parser.can_parse(url) {
            let res = parser.parse(client, url).await?;
            return Ok(ParseResult::Links(res));
        }
    }

    Ok(ParseResult::File(url.to_string()))
}
