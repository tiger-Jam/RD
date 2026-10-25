use anyhow::Result;
use async_trait::async_trait;

use crate::client::RateLimitedClient;

pub mod bms_search;
pub mod lr2ir_archive;
pub mod seed;
pub mod threads;

#[derive(Debug, Clone, Default)]
pub struct BmsUrl {
    pub main_urls: Vec<String>,
    pub diff_urls: Vec<String>,
}

#[async_trait]
pub trait BmsProvider: Send + Sync {
    fn name(&self) -> &'static str;
    async fn find_urls(&self, client: &RateLimitedClient, md5: &str) -> Result<BmsUrl>;
}
