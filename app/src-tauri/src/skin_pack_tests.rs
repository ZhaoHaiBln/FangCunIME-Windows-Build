use super::*;
use serde_json::Value;
use std::path::PathBuf;

fn vectors() -> (PathBuf, Vec<Value>) {
    let source = PathBuf::from(file!());
    let source = if source.is_absolute() {
        source
    } else {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(source)
    };
    let folder = source
        .ancestors()
        .map(|parent| parent.join("skin-format/test-vectors"))
        .find(|path| path.join("index.json").is_file())
        .expect("Shared .fcskin v1 vectors are required");
    let value: Value =
        serde_json::from_slice(&std::fs::read(folder.join("index.json")).unwrap()).unwrap();
    (folder, value["vectors"].as_array().unwrap().clone())
}

#[test]
fn shared_cross_platform_archive_vectors_match_the_fixed_v1_outcomes() {
    let (folder, cases) = vectors();
    assert!(
        cases.len() >= 80,
        "The full portable fixture set must be present"
    );
    for case in cases {
        let bytes = std::fs::read(folder.join(case["file"].as_str().unwrap())).unwrap();
        assert_eq!(bytes.len() as u64, case["bytes"].as_u64().unwrap());
        assert_eq!(
            format!("{:x}", Sha256::digest(&bytes)),
            case["sha256"].as_str().unwrap()
        );
        let result = validate_archive(&bytes);
        assert_eq!(
            result.is_ok(),
            case["valid"].as_bool().unwrap(),
            "{}: {}",
            case["name"],
            result.err().unwrap_or_default()
        );
    }
}

#[test]
fn installed_files_and_native_ini_retain_every_valid_shared_pack_without_wire_only_defaults() {
    let (folder, cases) = vectors();
    for case in cases.iter().filter(|case| case["valid"] == true) {
        let bytes = std::fs::read(folder.join(case["file"].as_str().unwrap())).unwrap();
        let pack = validate_archive(&bytes).unwrap();
        let reread = validate_files(&pack.files).unwrap();
        assert_eq!(
            serde_json::to_value(&reread).unwrap(),
            serde_json::to_value(&pack.manifest).unwrap()
        );
        let ini = resolved_ini(&pack).unwrap();
        assert!(ini.len() <= 16 * 1024 && !ini.contains('\r'));
        assert!(ini.contains(&format!("id={}\n", pack.manifest.id)));
        for mode in ["light", "dark"] {
            for key in COLORS {
                assert!(
                    ini.contains(&format!("{mode}_{}=", snake(key))),
                    "{mode} missing {key}"
                );
            }
            for index in 0..4 {
                assert!(ini.contains(&format!("{mode}_glow_{index}=")));
            }
        }
        let mut damaged = pack.files.clone();
        damaged.insert(
            "undeclared.png".into(),
            damaged[&pack.manifest.preview].clone(),
        );
        assert!(
            validate_files(&damaged).is_err(),
            "Installed cache revalidation cannot accept unregistered files"
        );
    }
}
