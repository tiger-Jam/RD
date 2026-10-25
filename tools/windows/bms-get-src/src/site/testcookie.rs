//! The "prove that you are human" page some hosts show first (k-bms and others): the page computes a cookie
//! with AES-CBC from three numbers written in it, then reloads. The same cookie is computed here.

use aes::Aes128;
use aes::cipher::{BlockCipherDecrypt, KeyInit};
use regex::Regex;
use std::sync::OnceLock;

/// The cookie the page would set ("NAME=hex"), if this is such a page.
pub fn solve(html: &str) -> Option<String> {
    if !html.contains("slowAES.decrypt") {
        return None;
    }
    static NUMS: OnceLock<Regex> = OnceLock::new();
    let re = NUMS.get_or_init(|| Regex::new(r#"(\w)=toNumbers\("([0-9a-fA-F]+)"\)"#).unwrap());
    let mut vals = std::collections::HashMap::new();
    for c in re.captures_iter(html) {
        vals.insert(c[1].to_string(), hex(&c[2])?);
    }
    // slowAES.decrypt(data, mode, key, iv)
    static CALL: OnceLock<Regex> = OnceLock::new();
    let call = CALL.get_or_init(|| Regex::new(r#"document\.cookie="(\w+)="\+toHex\(slowAES\.decrypt\((\w),\d+,(\w),(\w)\)\)"#).unwrap());
    let c = call.captures(html)?;
    let (data, key, iv) = (vals.get(&c[2])?, vals.get(&c[3])?, vals.get(&c[4])?);
    let key: [u8; 16] = key.as_slice().try_into().ok()?;
    let cipher = Aes128::new(&key.into());
    let mut prev: [u8; 16] = iv.as_slice().try_into().ok()?;
    let mut out = Vec::new();
    for block in data.chunks(16) {
        let mut b: [u8; 16] = block.try_into().ok()?;
        let cur = b;
        cipher.decrypt_block((&mut b).into());
        for (x, p) in b.iter_mut().zip(prev.iter()) {
            *x ^= p;
        }
        out.extend_from_slice(&b);
        prev = cur;
    }
    let value: String = out.iter().map(|b| format!("{:02x}", b)).collect();
    Some(format!("{}={}", &c[1], value))
}

fn hex(s: &str) -> Option<Vec<u8>> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(s.get(i..i + 2)?, 16).ok()).collect()
}
