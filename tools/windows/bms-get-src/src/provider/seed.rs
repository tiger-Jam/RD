use anyhow::Result;
use async_trait::async_trait;

use crate::client::RateLimitedClient;

use super::{BmsProvider, BmsUrl};

pub struct SeedProvider {
    urls: BmsUrl,
}

impl SeedProvider {
    pub fn new(urls: BmsUrl) -> Self {
        Self { urls }
    }
}

#[async_trait]
impl BmsProvider for SeedProvider {
    fn name(&self) -> &'static str {
        "Seed"
    }

    async fn find_urls(&self, _client: &RateLimitedClient, _md5: &str) -> Result<BmsUrl> {
        Ok(self.urls.clone())
    }
}
