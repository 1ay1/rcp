//! selector.rs — choose ONE backend from several reachable engines.
//!
//! Mirrors the C++/Python/Node `Selector`: list candidate engines (by hand or
//! from an `rcp.json` registry) and pick one — by id, by required capability, or
//! by priority with liveness fallback. Connection is lazy; the result is a
//! fully-connected `Client`.

use crate::client::Client;
use crate::json::Json;
use crate::types::{Capability, Errc, RcpError};

/// How to reach one backend. Inert until selected; exactly one of `argv` (stdio)
/// or `url` (HTTP) is set.
#[derive(Clone, Debug)]
pub struct EngineSpec {
    pub id: String,
    pub argv: Vec<String>,
    pub url: String,
    pub priority: i64,
    pub weight: f64,
    pub required: bool,
}

impl EngineSpec {
    fn open(&self) -> Result<Client, RcpError> {
        if !self.argv.is_empty() {
            Client::connect_stdio(&self.argv)
        } else if !self.url.is_empty() {
            Client::connect_http(&self.url)
        } else {
            Err(RcpError::new(
                Errc::INVALID_PARAMS,
                format!("engine '{}' has no transport", self.id),
            ))
        }
    }

    /// Parse one registry entry (spec §16.1).
    pub fn from_json(entry: &Json) -> Result<EngineSpec, RcpError> {
        let id = entry
            .get_str("id")
            .ok_or_else(|| RcpError::new(Errc::INVALID_PARAMS, "registry entry needs an 'id'"))?
            .to_string();
        let transport = entry.get_str("transport").unwrap_or("");
        let command = entry.get("command").or_else(|| entry.get("argv"));
        let url = entry.get_str("url").unwrap_or("");
        let mut spec = EngineSpec {
            id,
            argv: Vec::new(),
            url: String::new(),
            priority: entry.get("priority").and_then(|v| v.as_i64()).unwrap_or(0),
            weight: entry.get("weight").and_then(|v| v.as_f64()).unwrap_or(1.0),
            required: entry.get("required").and_then(|v| v.as_bool()).unwrap_or(false),
        };
        let has_command = command
            .and_then(|c| c.as_array())
            .is_some_and(|a| !a.is_empty());
        if transport == "stdio" || (transport.is_empty() && has_command) {
            let arr = command.and_then(|c| c.as_array()).ok_or_else(|| {
                RcpError::new(
                    Errc::INVALID_PARAMS,
                    format!("stdio engine '{}' needs a 'command' array", spec.id),
                )
            })?;
            spec.argv = arr.iter().filter_map(|v| v.as_str().map(String::from)).collect();
            if spec.argv.is_empty() {
                return Err(RcpError::new(
                    Errc::INVALID_PARAMS,
                    format!("stdio engine '{}' needs a non-empty 'command'", spec.id),
                ));
            }
        } else if transport == "http" || (transport.is_empty() && !url.is_empty()) {
            if url.is_empty() {
                return Err(RcpError::new(
                    Errc::INVALID_PARAMS,
                    format!("http engine '{}' needs a 'url'", spec.id),
                ));
            }
            spec.url = url.to_string();
        } else {
            return Err(RcpError::new(
                Errc::INVALID_PARAMS,
                format!("engine '{}' has no usable transport", spec.id),
            ));
        }
        Ok(spec)
    }
}

/// A registry of candidate engines; selection connects and returns a `Client`.
#[derive(Clone, Debug, Default)]
pub struct Selector {
    specs: Vec<EngineSpec>,
}

impl Selector {
    pub fn new() -> Selector {
        Selector { specs: Vec::new() }
    }

    /// Build a Selector from a registry JSON file.
    pub fn load(path: &str) -> Result<Selector, RcpError> {
        let text = std::fs::read_to_string(path)
            .map_err(|e| RcpError::new(Errc::INVALID_PARAMS, format!("cannot read {}: {}", path, e)))?;
        Selector::loads(&text)
    }

    /// Build a Selector from a registry JSON string (`{"engines": [ ... ]}`).
    pub fn loads(text: &str) -> Result<Selector, RcpError> {
        let doc = Json::parse(text)
            .map_err(|e| RcpError::new(Errc::PARSE_ERROR, format!("invalid registry JSON: {}", e)))?;
        let mut sel = Selector::new();
        if let Some(engines) = doc.get("engines").and_then(|v| v.as_array()) {
            for entry in engines {
                sel.specs.push(EngineSpec::from_json(entry)?);
            }
        }
        Ok(sel)
    }

    pub fn add_stdio(&mut self, id: &str, argv: &[&str], priority: i64) -> &mut Self {
        self.specs.push(EngineSpec {
            id: id.to_string(),
            argv: argv.iter().map(|s| s.to_string()).collect(),
            url: String::new(),
            priority,
            weight: 1.0,
            required: false,
        });
        self
    }

    pub fn add_http(&mut self, id: &str, url: &str, priority: i64) -> &mut Self {
        self.specs.push(EngineSpec {
            id: id.to_string(),
            argv: Vec::new(),
            url: url.to_string(),
            priority,
            weight: 1.0,
            required: false,
        });
        self
    }

    pub fn size(&self) -> usize {
        self.specs.len()
    }

    /// Connect the engine with the given id (or error if unknown).
    pub fn select(&self, id: &str) -> Result<Client, RcpError> {
        for spec in &self.specs {
            if spec.id == id {
                return spec.open();
            }
        }
        Err(RcpError::new(
            Errc::INVALID_PARAMS,
            format!("no engine with id '{}'", id),
        ))
    }

    /// Connect the highest-priority engine that answers (liveness fallback).
    pub fn select_primary(&self) -> Result<Client, RcpError> {
        let mut last: Option<RcpError> = None;
        for spec in self.by_priority() {
            match spec.open() {
                Ok(c) => return Ok(c),
                Err(e) => last = Some(e),
            }
        }
        Err(last.unwrap_or_else(|| RcpError::new(Errc::BACKEND_UNAVAILABLE, "no engines configured")))
    }

    /// Connect the highest-priority engine that answers AND advertises `cap`.
    pub fn select_capable(&self, cap: Capability) -> Result<Client, RcpError> {
        let mut last: Option<RcpError> = None;
        for spec in self.by_priority() {
            match spec.open() {
                Ok(mut c) => {
                    if c.supports(cap) {
                        return Ok(c);
                    }
                    c.shutdown();
                    last = Some(RcpError::new(
                        Errc::CAPABILITY_MISSING,
                        format!("engine '{}' lacks '{}'", spec.id, cap.wire_key()),
                    ));
                }
                Err(e) => last = Some(e),
            }
        }
        Err(last.unwrap_or_else(|| RcpError::new(Errc::BACKEND_UNAVAILABLE, "no capable engine")))
    }

    /// Specs sorted by descending priority (stable for equal priority).
    fn by_priority(&self) -> Vec<&EngineSpec> {
        let mut v: Vec<&EngineSpec> = self.specs.iter().collect();
        v.sort_by(|a, b| b.priority.cmp(&a.priority));
        v
    }
}
