use governor::{
    Quota, RateLimiter,
    clock::DefaultClock,
    state::{InMemoryState, NotKeyed},
};
use reqwest::{Client, IntoUrl, RequestBuilder};
use std::sync::Arc;

type SharedLimiter = Arc<RateLimiter<NotKeyed, InMemoryState, DefaultClock>>;

#[derive(Clone)]
pub struct RateLimitedClient {
    client: Client,
    limiter: SharedLimiter,
}

impl RateLimitedClient {
    pub fn new(client: Client, quota: Quota) -> Self {
        Self {
            client,
            limiter: Arc::new(RateLimiter::direct(quota)),
        }
    }

    pub async fn get<U: IntoUrl>(&self, url: U) -> RequestBuilder {
        self.limiter.until_ready().await;
        self.client.get(url)
    }

    pub async fn post<U: IntoUrl>(&self, url: U) -> RequestBuilder {
        self.limiter.until_ready().await;
        self.client.post(url)
    }
}
