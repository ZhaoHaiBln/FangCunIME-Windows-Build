//! Internal edits layer over the four existing renderers. This is deliberately
//! separate from the fixed, portable `.fcskin v1` archive format.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

pub const KEY: &str = "builtin_skin_edits_v1";
pub const INVALID: &str = "内置皮肤编辑配置无效，已保留原设置";
pub const MAX_BYTES: usize = 32 * 1024;
pub const IDS: [&str; 4] = ["wood_type", "typewriter", "terminal", "brutal"];

#[derive(Clone, Default, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct BuiltinTone {
    #[serde(
        default,
        deserialize_with = "unique_map",
        skip_serializing_if = "BTreeMap::is_empty"
    )]
    pub colors: BTreeMap<String, String>,
    #[serde(
        default,
        deserialize_with = "present",
        skip_serializing_if = "Option::is_none"
    )]
    pub glow: Option<Vec<String>>,
}

#[derive(Clone, Default, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct BuiltinStyle {
    #[serde(
        default,
        deserialize_with = "present",
        skip_serializing_if = "Option::is_none"
    )]
    pub font_family: Option<String>,
    #[serde(
        default,
        deserialize_with = "present",
        skip_serializing_if = "Option::is_none"
    )]
    pub press_offset_dp: Option<f64>,
    #[serde(
        default,
        deserialize_with = "unique_map",
        skip_serializing_if = "BTreeMap::is_empty"
    )]
    pub themes: BTreeMap<String, BuiltinTone>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BuiltinEdits {
    pub version: u32,
    #[serde(deserialize_with = "unique_map")]
    pub styles: BTreeMap<String, BuiltinStyle>,
    #[serde(skip)]
    pub raw: Option<String>,
}
impl Default for BuiltinEdits {
    fn default() -> Self {
        Self {
            version: 1,
            styles: BTreeMap::new(),
            raw: None,
        }
    }
}

fn present<'de, D, T>(deserializer: D) -> Result<Option<T>, D::Error>
where
    D: serde::Deserializer<'de>,
    T: Deserialize<'de>,
{
    T::deserialize(deserializer).map(Some)
}
fn unique_map<'de, D, T>(deserializer: D) -> Result<BTreeMap<String, T>, D::Error>
where
    D: serde::Deserializer<'de>,
    T: Deserialize<'de>,
{
    struct Unique<T>(std::marker::PhantomData<T>);
    impl<'de, T: Deserialize<'de>> serde::de::Visitor<'de> for Unique<T> {
        type Value = BTreeMap<String, T>;
        fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
            f.write_str("an object with unique keys")
        }
        fn visit_map<A: serde::de::MapAccess<'de>>(
            self,
            mut map: A,
        ) -> Result<Self::Value, A::Error> {
            let mut out = BTreeMap::new();
            while let Some((key, value)) = map.next_entry::<String, T>()? {
                if out.insert(key, value).is_some() {
                    return Err(serde::de::Error::custom("duplicate field"));
                }
            }
            Ok(out)
        }
    }
    deserializer.deserialize_map(Unique(std::marker::PhantomData))
}
pub fn valid_id(id: &str) -> bool {
    IDS.contains(&id)
}
pub fn caption_defaults(settings: &crate::settings::AppSettings) -> (u32, u32) {
    match (settings.skin.as_str(), settings.dark_mode) {
        ("typewriter", false) => (0xEBDDCA, 0x272019),
        ("typewriter", true) => (0x302923, 0xF6E8D1),
        ("terminal", false) => (0xD7CDBA, 0x252722),
        ("terminal", true) => (0x34382F, 0xF0ECDB),
        ("brutal", false) => (0xEFEAE0, 0x121212),
        ("brutal", true) => (0x0F1412, 0xE6F4EC),
        (_, true) => (0x3C2D24, 0xF5E3CC),
        _ => (0xE8D6BA, 0x251B14),
    }
}
fn color(value: &str) -> bool {
    value.len() == 7
        && value.starts_with('#')
        && value.as_bytes()[1..].iter().all(u8::is_ascii_hexdigit)
}
impl BuiltinStyle {
    pub fn validate(&self) -> Result<(), String> {
        if self
            .font_family
            .as_deref()
            .is_some_and(|f| !matches!(f, "sans" | "serif" | "mono"))
            || self
                .press_offset_dp
                .is_some_and(|p| !p.is_finite() || !(0.0..=2.0).contains(&p))
            || self.themes.iter().any(|(mode, tone)| {
                !matches!(mode.as_str(), "light" | "dark")
                    || tone.colors.iter().any(|(name, value)| {
                        !crate::skin_pack::COLORS.contains(&name.as_str()) || !color(value)
                    })
                    || tone
                        .glow
                        .as_ref()
                        .is_some_and(|g| g.len() != 4 || g.iter().any(|v| !color(v)))
            })
        {
            return Err(INVALID.into());
        }
        if serde_json::to_vec(self).map_err(|_| INVALID)?.len() > MAX_BYTES {
            return Err(INVALID.into());
        }
        Ok(())
    }
    pub fn hash(&self, id: &str) -> Result<String, String> {
        self.validate()?;
        if !valid_id(id) {
            return Err(INVALID.into());
        }
        let mut sha = Sha256::new();
        sha.update(id.as_bytes());
        sha.update([0]);
        sha.update(serde_json::to_vec(self).map_err(|_| INVALID)?);
        Ok(format!("{:x}", sha.finalize()))
    }
    pub fn resolved_ini(&self, id: &str) -> Result<String, String> {
        let hash = self.hash(id)?;
        let mut out = format!("schema_version=1\nbuiltin={id}\nhash={hash}\n");
        if let Some(font) = &self.font_family {
            out += &format!("font_family={font}\n");
        }
        if let Some(press) = self.press_offset_dp {
            // JSON's round-trippable compact spelling also keeps subnormal
            // finite offsets within the native reader's per-line budget.
            let value = serde_json::to_string(&press).map_err(|_| INVALID)?;
            out += &format!("press_offset_dp={value}\n");
        }
        for (mode, tone) in &self.themes {
            for (name, value) in &tone.colors {
                out += &format!("{mode}_{}={value}\n", snake(name));
            }
            if let Some(glow) = &tone.glow {
                for (i, color) in glow.iter().enumerate() {
                    out += &format!("{mode}_glow_{i}={color}\n");
                }
            }
        }
        if out.len() > 16384 {
            return Err(INVALID.into());
        }
        Ok(out)
    }
}
fn snake(value: &str) -> String {
    value
        .chars()
        .flat_map(|c| {
            if c.is_ascii_uppercase() {
                vec!['_', c.to_ascii_lowercase()]
            } else {
                vec![c]
            }
        })
        .collect()
}
impl BuiltinEdits {
    pub fn from_document(document: &str) -> Result<Self, String> {
        let raw = document
            .lines()
            .filter_map(|line| {
                let line = line.trim().trim_start_matches('\u{feff}');
                if line.starts_with(['#', ';']) {
                    return None;
                }
                let (key, value) = line.split_once('=')?;
                (key.trim() == KEY).then_some(value.trim())
            })
            .next_back();
        let Some(raw) = raw else {
            return Ok(Self::default());
        };
        if raw.len() > MAX_BYTES {
            return Err(INVALID.into());
        }
        let mut out: Self = serde_json::from_str(raw).map_err(|_| INVALID)?;
        if out.version != 1 || out.styles.len() > 4 || out.styles.keys().any(|id| !valid_id(id)) {
            return Err(INVALID.into());
        }
        for style in out.styles.values() {
            style.validate()?;
        }
        out.raw = Some(raw.into());
        Ok(out)
    }
    pub fn encode(&self) -> Result<String, String> {
        let text = serde_json::to_string(self).map_err(|_| INVALID)?;
        if text.len() > MAX_BYTES {
            return Err(INVALID.into());
        }
        Ok(text)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn strict_partial_styles_reject_unknown_duplicate_null_and_bad_values() {
        let valid = r##"{"fontFamily":"sans","themes":{"dark":{"colors":{"surface":"#123456"},"glow":["#123456","#ffffff","#000000","#aabbcc"]}}}"##;
        let style: BuiltinStyle = serde_json::from_str(valid).unwrap();
        style.validate().unwrap();
        for raw in [
            r#"{"fontFamily":null}"#,
            r#"{"pressOffsetDp":null}"#,
            r#"{"fontFamily":"sans","fontFamily":"mono"}"#,
            r#"{"photos":{}}"#,
            r#"{"themes":{"light":{},"light":{}}}"#,
            r##"{"themes":{"light":{"colors":{"text":"#123456","text":"#ffffff"}}}}"##,
        ] {
            assert!(serde_json::from_str::<BuiltinStyle>(raw).is_err(), "{raw}");
        }
        for raw in [
            r#"{"fontFamily":"bogus"}"#,
            r#"{"pressOffsetDp":2.01}"#,
            r#"{"themes":{"system":{}}}"#,
            r##"{"themes":{"light":{"colors":{"text":"#123"}}}}"##,
            r##"{"themes":{"light":{"colors":{"unknown":"#123456"}}}}"##,
            r##"{"themes":{"dark":{"glow":["#123456"]}}}"##,
        ] {
            assert!(
                serde_json::from_str::<BuiltinStyle>(raw)
                    .unwrap()
                    .validate()
                    .is_err(),
                "{raw}"
            );
        }
    }
    #[test]
    fn absent_defaults_and_canonical_hash_preserve_only_the_selected_skin_and_mode() {
        assert!(BuiltinEdits::from_document("skin=brutal\n")
            .unwrap()
            .styles
            .is_empty());
        let style: BuiltinStyle = serde_json::from_str(
            r##"{"pressOffsetDp":0.5,"themes":{"light":{"colors":{"onSelection":"#123456"}}}}"##,
        )
        .unwrap();
        let ini = style.resolved_ini("brutal").unwrap();
        assert!(
            ini.contains("light_on_selection=#123456\n")
                && !ini.contains("dark_")
                && !ini.contains("font_family=")
        );
        assert_ne!(
            style.hash("brutal").unwrap(),
            style.hash("terminal").unwrap()
        );
        let tiny = BuiltinStyle {
            press_offset_dp: Some(f64::from_bits(1)),
            ..BuiltinStyle::default()
        };
        assert!(tiny
            .resolved_ini("brutal")
            .unwrap()
            .lines()
            .all(|line| line.len() <= 160));
        assert!(BuiltinEdits::from_document(
            r#"builtin_skin_edits_v1={"version":1.0,"styles":{}}"#
        )
        .is_err());
        assert!(BuiltinEdits::from_document(
            r#"builtin_skin_edits_v1={"version":1,"styles":{"unknown":{}}}"#
        )
        .is_err());
    }
}
