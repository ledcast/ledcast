use std::path::PathBuf;

use serde::{Deserialize, Serialize};

use crate::canvas::{Fit, Transform};
use crate::stream::DEFAULT_PORT;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Node {
    pub id: String,
    pub name: String,
    pub host: String,
    pub width: u16,
    pub height: u16,
    #[serde(default)]
    pub modules: u16,
    #[serde(default)]
    pub firmware: String,
    #[serde(default)]
    pub rssi: Option<i16>,
    #[serde(default = "default_bit_depth")]
    pub bit_depth: u8,
    /// Частота кадров вывода строки: и видео, и автономный показ идут по ней.
    #[serde(default = "default_frame_rate")]
    pub frame_rate: u16,
}

fn default_bit_depth() -> u8 {
    8
}

fn default_frame_rate() -> u16 {
    120
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Mapping {
    pub node_id: String,
    #[serde(default)]
    pub ndi_source: Option<String>,
    #[serde(default)]
    pub fit: Fit,
    #[serde(default)]
    pub transform: Transform,
    #[serde(default = "default_enabled")]
    pub enabled: bool,
}

fn default_enabled() -> bool {
    true
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Settings {
    pub nodes: Vec<Node>,
    pub mappings: Vec<Mapping>,
    pub manual_host: String,
    pub stream_port: u16,
    pub autostart: bool,
    /// Задержка показа, мс: одна на все строки (`feed::DEFAULT_LATENCY_MS`).
    pub latency_ms: u32,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            nodes: Vec::new(),
            mappings: Vec::new(),
            manual_host: String::new(),
            stream_port: DEFAULT_PORT,
            autostart: true,
            latency_ms: crate::feed::DEFAULT_LATENCY_MS,
        }
    }
}

impl Settings {
    pub fn load() -> Self {
        settings_path()
            .and_then(|path| std::fs::read_to_string(path).ok())
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_default()
    }

    pub fn save(&self) {
        let Some(path) = settings_path() else { return };
        if let Some(parent) = path.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        if let Ok(text) = serde_json::to_string_pretty(self) {
            let _ = std::fs::write(path, text);
        }
    }

    pub fn mapping_mut(&mut self, node_id: &str) -> &mut Mapping {
        if let Some(index) = self
            .mappings
            .iter()
            .position(|item| item.node_id == node_id)
        {
            return &mut self.mappings[index];
        }
        self.mappings.push(Mapping {
            node_id: node_id.to_owned(),
            ndi_source: None,
            fit: Fit::Cover,
            transform: Transform::default(),
            enabled: true,
        });
        self.mappings.last_mut().unwrap()
    }
}

fn settings_path() -> Option<PathBuf> {
    Some(data_dir()?.join("console.json"))
}

/// Папка данных Console: настройки и ключи строк.
pub fn data_dir() -> Option<PathBuf> {
    let base = if cfg!(target_os = "windows") {
        std::env::var_os("APPDATA").map(PathBuf::from)
    } else if cfg!(target_os = "macos") {
        std::env::var_os("HOME").map(|home| PathBuf::from(home).join("Library/Application Support"))
    } else {
        std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))
    }?;
    Some(base.join("LEDCAST"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn creates_one_mapping_per_node() {
        let mut settings = Settings::default();
        settings.mapping_mut("line-1").enabled = false;
        settings.mapping_mut("line-1").fit = Fit::Contain;
        assert_eq!(settings.mappings.len(), 1);
        assert_eq!(settings.mappings[0].fit, Fit::Contain);
    }
}
