use anyhow::Result;
use async_trait::async_trait;
use std::path::PathBuf;

use crate::client::RateLimitedClient;
use crate::threads::Index;

use super::{BmsProvider, BmsUrl};

/// Links posted on the request boards for the song (by title and artist; the boards do not know md5).
pub struct ThreadsProvider {
    pub index: PathBuf,
    pub title: String,
    pub artist: Option<String>,
}

#[async_trait]
impl BmsProvider for ThreadsProvider {
    fn name(&self) -> &'static str {
        "Request boards"
    }

    async fn find_urls(&self, _client: &RateLimitedClient, _md5: &str) -> Result<BmsUrl> {
        let index = Index::load(&self.index)?;
        let urls = index.find(&self.title, self.artist.as_deref());
        log::info!("Request boards: {} links for {} / {}", urls.len(), self.title, self.artist.as_deref().unwrap_or("?"));
        for u in &urls {
            log::debug!("  {}", u);
        }
        // The boards rarely say which is the body and which the diff: the walker validates whatever comes down
        Ok(BmsUrl { main_urls: urls, diff_urls: vec![] })
    }
}
