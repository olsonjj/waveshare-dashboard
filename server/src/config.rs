//! config.rs — server-local configuration (TOML).
//!
//! `server/config.toml` is gitignored (secrets); `config.example.toml`
//! is the committed template. Envoy host + bearer token must never be
//! committed (they used to live in the firmware's `secrets.h`).

use serde::Deserialize;

#[derive(Debug, Deserialize)]
pub struct Config {
    pub envoy_host: String,
    pub envoy_token: String,
    #[serde(default = "default_listen_port")]
    pub listen_port: u16,
    #[serde(default = "default_db_path")]
    pub db_path: String,
    #[serde(default = "default_poll_interval_s")]
    pub poll_interval_s: u64,
}

fn default_listen_port() -> u16 {
    8421
}
fn default_db_path() -> String {
    "data/solar.db".to_string()
}
fn default_poll_interval_s() -> u64 {
    60
}

/// Load the config file at `path`. The error string is printed verbatim
/// at startup so a missing config tells the operator exactly what to do.
pub fn load(path: &str) -> Result<Config, String> {
    let raw = std::fs::read_to_string(path).map_err(|e| {
        format!("cannot read {path}: {e} — copy config.example.toml to config.toml and fill it in")
    })?;
    let cfg: Config = toml::from_str(&raw).map_err(|e| format!("cannot parse {path}: {e}"))?;
    if cfg.envoy_host.trim().is_empty() {
        return Err(format!("{path}: envoy_host is empty"));
    }
    if cfg.envoy_token.trim().is_empty() {
        return Err(format!("{path}: envoy_token is empty"));
    }
    Ok(cfg)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_full_config() {
        let cfg = load("config.example.toml").expect("example config parses");
        assert!(!cfg.envoy_host.is_empty());
        assert!(!cfg.envoy_token.is_empty());
        assert_eq!(cfg.listen_port, 8421);
        assert_eq!(cfg.db_path, "data/solar.db");
        assert_eq!(cfg.poll_interval_s, 60);
    }

    #[test]
    fn minimal_config_uses_defaults() {
        std::fs::write(
            "/tmp/solar-push-test-minimal.toml",
            "envoy_host = \"192.168.1.5\"\nenvoy_token = \"t\"\n",
        )
        .unwrap();
        let cfg = load("/tmp/solar-push-test-minimal.toml").unwrap();
        assert_eq!(cfg.listen_port, 8421);
        assert_eq!(cfg.db_path, "data/solar.db");
        assert_eq!(cfg.poll_interval_s, 60);
    }

    #[test]
    fn missing_or_empty_credentials_are_rejected() {
        assert!(load("/tmp/solar-push-test-nonexistent.toml").is_err());
        std::fs::write(
            "/tmp/solar-push-test-empty.toml",
            "envoy_host = \"\"\nenvoy_token = \"t\"\n",
        )
        .unwrap();
        assert!(load("/tmp/solar-push-test-empty.toml").is_err());
    }
}
