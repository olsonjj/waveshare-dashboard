//! envoy.rs — Enphase IQ Gateway poller (host side).
//!
//! GET https://<envoy>/ivp/pdm/energy every 60 s, Authorization: Bearer.
//! The gateway serves TLS with a SELF-SIGNED cert; it is a verified
//! device on our own LAN, so certificate verification is intentionally
//! disabled (the same trust decision the board's direct path and the
//! retired relay made).
//!
//! `watts_peak_today` is NOT read from the Envoy: the server tracks
//! max(watts_now) observed today (spec "Implementation Decisions"); the
//! inverters endpoint is out of scope this version.

use std::time::Duration;

use serde::Deserialize;

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
struct Pcu {
    #[serde(default)]
    watts_now: u32,
    #[serde(default)]
    watt_hours_today: u32,
    #[serde(default)]
    watt_hours_seven_days: u32,
}

#[derive(Debug, Default, Deserialize)]
struct Production {
    #[serde(default)]
    pcu: Pcu,
}

#[derive(Debug, Default, Deserialize)]
struct EnergyResponse {
    #[serde(default)]
    production: Production,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EnvoyReading {
    pub watts_now: u32,
    pub wh_today: u32,
    pub wh_7day: u32,
}

/// Parse the /ivp/pdm/energy response body. Missing fields count as 0
/// (the same defaulting the board's ArduinoJson filter performed).
pub fn parse_energy_json(body: &str) -> Result<EnvoyReading, String> {
    let r: EnergyResponse =
        serde_json::from_str(body).map_err(|e| format!("energy JSON parse failed: {e}"))?;
    Ok(EnvoyReading {
        watts_now: r.production.pcu.watts_now,
        wh_today: r.production.pcu.watt_hours_today,
        wh_7day: r.production.pcu.watt_hours_seven_days,
    })
}

pub struct EnvoyClient {
    host: String,
    token: String,
    http: reqwest::blocking::Client,
}

impl EnvoyClient {
    pub fn new(host: &str, token: &str) -> Self {
        let http = reqwest::blocking::Client::builder()
            .danger_accept_invalid_certs(true) // self-signed LAN gateway
            .timeout(Duration::from_secs(8))
            .build()
            .expect("reqwest client builds");
        EnvoyClient {
            host: host.to_string(),
            token: token.to_string(),
            http,
        }
    }

    /// One poll. Returns the reading or a human-readable error (logged,
    /// retried on the next 60 s tick; the push keeps serving cached state).
    pub fn fetch(&self) -> Result<EnvoyReading, String> {
        let url = format!("https://{}/ivp/pdm/energy", self.host);
        let resp = self
            .http
            .get(&url)
            .bearer_auth(&self.token)
            .header("Accept", "application/json")
            .send()
            .map_err(|e| format!("envoy request failed: {e}"))?;
        if !resp.status().is_success() {
            return Err(format!("envoy HTTP status {}", resp.status()));
        }
        let body = resp
            .text()
            .map_err(|e| format!("envoy body read failed: {e}"))?;
        parse_energy_json(&body)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const FIXTURE: &str = r#"{
      "production": {
        "type": "PCU",
        "activeCount": 12,
        "pcu": {
          "wattsNow": 3840,
          "wattHoursToday": 21651,
          "wattHoursSevenDays": 987650,
          "wattHoursLifetime": 23456789
        },
        "acb": { "wattsNow": 0, "wattHoursToday": 0, "wattHoursSevenDays": 0 }
      },
      "consumption": { "standard": { "wattsNow": 0 } }
    }"#;

    #[test]
    fn parses_fixture_into_reading() {
        let r = parse_energy_json(FIXTURE).unwrap();
        assert_eq!(
            r,
            EnvoyReading {
                watts_now: 3840,
                wh_today: 21651,
                wh_7day: 987_650
            }
        );
    }

    #[test]
    fn missing_fields_default_to_zero() {
        let r = parse_energy_json(r#"{"production":{"pcu":{"wattsNow":42}}}"#).unwrap();
        assert_eq!(r.watts_now, 42);
        assert_eq!(r.wh_today, 0);
        assert_eq!(r.wh_7day, 0);
    }

    #[test]
    fn garbage_body_is_an_error() {
        assert!(parse_energy_json("<html>not json</html>").is_err());
        assert!(parse_energy_json("").is_err());
    }
}
