// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashSet;
use std::fmt;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Component, Path, PathBuf};

use crate::sha256::digest_hex;
use zeroize::Zeroize;

pub const MANIFEST_FILE: &str = "manifest.kfaceauth";
pub const MANIFEST_MAGIC: &str = "KFACEAUTH_MODEL_MANIFEST\t1";
pub const MANIFEST_HEADER: &str = "id\tpath\tsize\tsha256\trole\tbackend\tlicense\tprovenance";
pub const MAX_MANIFEST_BYTES: u64 = 64 * 1024;
pub const MAX_ARTIFACT_BYTES: u64 = 64 * 1024 * 1024;
pub const MAX_MANIFEST_ENTRIES: usize = 64;
const MAX_INVENTORY_ENTRIES: usize = MAX_MANIFEST_ENTRIES * 2 + 1;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ManifestEntry {
    pub id: String,
    pub path: PathBuf,
    pub size: u64,
    pub sha256: String,
    pub role: String,
    pub backend: String,
    pub license: String,
    pub provenance: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ModelManifest {
    entries: Vec<ManifestEntry>,
}

impl ModelManifest {
    /// Parses the closed, tab-separated model manifest schema.
    ///
    /// # Errors
    ///
    /// Returns [`ModelError::InvalidManifest`] for any unknown field layout,
    /// unsafe path, duplicate, unsorted entry, invalid size, or invalid digest.
    pub fn parse(text: &str) -> Result<Self, ModelError> {
        if text.contains('\r') || !text.ends_with('\n') {
            return Err(ModelError::InvalidManifest);
        }
        let mut lines = text.lines();
        if lines.next() != Some(MANIFEST_MAGIC) || lines.next() != Some(MANIFEST_HEADER) {
            return Err(ModelError::InvalidManifest);
        }

        let mut entries = Vec::new();
        let mut ids = HashSet::new();
        let mut paths = HashSet::new();
        let mut previous_path: Option<String> = None;
        for line in lines {
            if line.is_empty() {
                return Err(ModelError::InvalidManifest);
            }
            let fields: Vec<&str> = line.split('\t').collect();
            let [id, path, size, sha256, role, backend, license, provenance] = fields.as_slice()
            else {
                return Err(ModelError::InvalidManifest);
            };
            if !valid_token(id)
                || !valid_token(role)
                || !valid_token(backend)
                || !valid_token(license)
                || !valid_token(provenance)
                || !valid_relative_path(path)
                || !valid_sha256(sha256)
            {
                return Err(ModelError::InvalidManifest);
            }
            let size = size
                .parse::<u64>()
                .map_err(|_| ModelError::InvalidManifest)?;
            if size == 0 || size > MAX_ARTIFACT_BYTES {
                return Err(ModelError::InvalidManifest);
            }
            if previous_path
                .as_deref()
                .is_some_and(|previous| previous >= *path)
                || !ids.insert((*id).to_owned())
                || !paths.insert((*path).to_owned())
            {
                return Err(ModelError::InvalidManifest);
            }
            previous_path = Some((*path).to_owned());
            entries.push(ManifestEntry {
                id: (*id).to_owned(),
                path: PathBuf::from(path),
                size,
                sha256: (*sha256).to_owned(),
                role: (*role).to_owned(),
                backend: (*backend).to_owned(),
                license: (*license).to_owned(),
                provenance: (*provenance).to_owned(),
            });
            if entries.len() > MAX_MANIFEST_ENTRIES {
                return Err(ModelError::InvalidManifest);
            }
        }
        if entries.is_empty() {
            return Err(ModelError::InvalidManifest);
        }
        Ok(Self { entries })
    }

    #[must_use]
    pub fn entries(&self) -> &[ManifestEntry] {
        &self.entries
    }

    #[must_use]
    pub fn find(&self, id: &str) -> Option<&ManifestEntry> {
        self.entries.iter().find(|entry| entry.id == id)
    }
}

#[derive(Debug)]
pub struct VerifiedArtifact {
    entry: ManifestEntry,
    bytes: Vec<u8>,
}

impl VerifiedArtifact {
    #[must_use]
    pub fn entry(&self) -> &ManifestEntry {
        &self.entry
    }

    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
}

impl Drop for VerifiedArtifact {
    fn drop(&mut self) {
        self.bytes.zeroize();
    }
}

#[derive(Debug)]
pub enum ModelError {
    Io(io::Error),
    InvalidManifest,
    UnsafeFilesystemEntry,
    ArtifactNotFound,
    InventoryMismatch,
    ArtifactSizeMismatch,
    ArtifactDigestMismatch,
}

impl fmt::Display for ModelError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => write!(formatter, "model supply-chain I/O failed: {error}"),
            Self::InvalidManifest => formatter.write_str("model manifest is invalid"),
            Self::UnsafeFilesystemEntry => {
                formatter.write_str("model supply chain contains a symlink or non-regular file")
            }
            Self::ArtifactNotFound => formatter.write_str("model artifact is not listed"),
            Self::InventoryMismatch => {
                formatter.write_str("model artifact inventory does not match the manifest")
            }
            Self::ArtifactSizeMismatch => formatter.write_str("model artifact size mismatch"),
            Self::ArtifactDigestMismatch => formatter.write_str("model artifact digest mismatch"),
        }
    }
}

impl std::error::Error for ModelError {}

impl From<io::Error> for ModelError {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

/// Loads a manifest only after checking its filesystem type and byte bound.
///
/// # Errors
///
/// Returns [`ModelError`] when the root or manifest is unsafe, unreadable,
/// oversized, non-UTF-8, or invalid.
pub fn load_manifest(root: &Path) -> Result<ModelManifest, ModelError> {
    require_directory(root)?;
    let path = root.join(MANIFEST_FILE);
    let metadata = fs::symlink_metadata(&path)?;
    if !metadata.file_type().is_file() || metadata.file_type().is_symlink() {
        return Err(ModelError::UnsafeFilesystemEntry);
    }
    if metadata.len() == 0 || metadata.len() > MAX_MANIFEST_BYTES {
        return Err(ModelError::InvalidManifest);
    }
    let mut bytes = Vec::with_capacity(
        usize::try_from(metadata.len()).map_err(|_| ModelError::InvalidManifest)?,
    );
    if let Err(error) = File::open(path)?
        .take(MAX_MANIFEST_BYTES + 1)
        .read_to_end(&mut bytes)
    {
        bytes.zeroize();
        return Err(ModelError::Io(error));
    }
    if u64::try_from(bytes.len()) != Ok(metadata.len()) {
        bytes.zeroize();
        return Err(ModelError::InvalidManifest);
    }
    let Ok(text) = std::str::from_utf8(&bytes) else {
        bytes.zeroize();
        return Err(ModelError::InvalidManifest);
    };
    let result = ModelManifest::parse(text);
    bytes.zeroize();
    result
}

/// Reads a listed artifact into memory and verifies its exact size and SHA-256.
///
/// The returned bytes are the same bytes whose digest was checked. They are
/// cleared when [`VerifiedArtifact`] is dropped.
///
/// # Errors
///
/// Returns [`ModelError`] for an invalid manifest, unknown ID, unsafe path,
/// read failure, or integrity mismatch.
pub fn load_verified_artifact(root: &Path, id: &str) -> Result<VerifiedArtifact, ModelError> {
    let manifest = load_manifest(root)?;
    let entry = manifest
        .find(id)
        .ok_or(ModelError::ArtifactNotFound)?
        .clone();
    load_verified_entry(root, entry)
}

/// Verifies the complete closed model inventory and every listed digest.
///
/// # Errors
///
/// Returns [`ModelError`] when the manifest is invalid, a listed artifact is
/// missing or modified, an unlisted filesystem entry is present, or any path
/// is not a regular non-symlink file or directory.
pub fn verify_model_root(root: &Path) -> Result<ModelManifest, ModelError> {
    let (manifest, _) = load_and_verify_model_inventory(root)?;
    Ok(manifest)
}

/// Verifies the model root and returns all loaded verified artifacts in one pass.
///
/// # Errors
///
/// Returns [`ModelError`] when the manifest is invalid, a listed artifact is
/// missing or modified, an unlisted filesystem entry is present, or any path
/// is not a regular non-symlink file or directory.
pub fn load_and_verify_model_inventory(
    root: &Path,
) -> Result<(ModelManifest, Vec<VerifiedArtifact>), ModelError> {
    let manifest = load_manifest(root)?;
    let listed: HashSet<PathBuf> = manifest
        .entries()
        .iter()
        .map(|entry| entry.path.clone())
        .collect();
    let mut present = regular_files(root)?;
    present.remove(Path::new(MANIFEST_FILE));
    if present != listed {
        return Err(ModelError::InventoryMismatch);
    }
    let mut artifacts = Vec::with_capacity(manifest.entries().len());
    for entry in manifest.entries() {
        artifacts.push(load_verified_entry(root, entry.clone())?);
    }
    Ok((manifest, artifacts))
}

fn load_verified_entry(root: &Path, entry: ManifestEntry) -> Result<VerifiedArtifact, ModelError> {
    reject_symlink_components(root, &entry.path)?;
    let path = root.join(&entry.path);
    let metadata = fs::symlink_metadata(&path)?;
    if !metadata.file_type().is_file() || metadata.file_type().is_symlink() {
        return Err(ModelError::UnsafeFilesystemEntry);
    }
    if metadata.len() != entry.size {
        return Err(ModelError::ArtifactSizeMismatch);
    }

    let mut bytes = Vec::with_capacity(
        usize::try_from(entry.size).map_err(|_| ModelError::ArtifactSizeMismatch)?,
    );
    if let Err(error) = File::open(path)?
        .take(entry.size + 1)
        .read_to_end(&mut bytes)
    {
        bytes.zeroize();
        return Err(ModelError::Io(error));
    }
    if u64::try_from(bytes.len()) != Ok(entry.size) {
        bytes.zeroize();
        return Err(ModelError::ArtifactSizeMismatch);
    }
    if digest_hex(&bytes) != entry.sha256 {
        bytes.zeroize();
        return Err(ModelError::ArtifactDigestMismatch);
    }
    Ok(VerifiedArtifact { entry, bytes })
}

fn regular_files(root: &Path) -> Result<HashSet<PathBuf>, ModelError> {
    require_directory(root)?;
    let mut directories = vec![root.to_path_buf()];
    let mut files = HashSet::new();
    let mut entry_count = 0_usize;
    while let Some(directory) = directories.pop() {
        for entry in fs::read_dir(directory)? {
            let entry = entry?;
            entry_count = entry_count
                .checked_add(1)
                .ok_or(ModelError::InventoryMismatch)?;
            if entry_count > MAX_INVENTORY_ENTRIES {
                return Err(ModelError::InventoryMismatch);
            }
            let path = entry.path();
            let metadata = fs::symlink_metadata(&path)?;
            if metadata.file_type().is_symlink() {
                return Err(ModelError::UnsafeFilesystemEntry);
            }
            if metadata.file_type().is_dir() {
                directories.push(path);
            } else if metadata.file_type().is_file() {
                let relative = path
                    .strip_prefix(root)
                    .map_err(|_| ModelError::UnsafeFilesystemEntry)?
                    .to_path_buf();
                if !files.insert(relative) {
                    return Err(ModelError::InventoryMismatch);
                }
            } else {
                return Err(ModelError::UnsafeFilesystemEntry);
            }
        }
    }
    Ok(files)
}

fn require_directory(path: &Path) -> Result<(), ModelError> {
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.file_type().is_dir() || metadata.file_type().is_symlink() {
        return Err(ModelError::UnsafeFilesystemEntry);
    }
    Ok(())
}

fn reject_symlink_components(root: &Path, relative: &Path) -> Result<(), ModelError> {
    let mut current = root.to_path_buf();
    for component in relative.components() {
        let Component::Normal(segment) = component else {
            return Err(ModelError::InvalidManifest);
        };
        current.push(segment);
        let metadata = fs::symlink_metadata(&current)?;
        if metadata.file_type().is_symlink() {
            return Err(ModelError::UnsafeFilesystemEntry);
        }
    }
    Ok(())
}

fn valid_relative_path(value: &str) -> bool {
    if value.is_empty() || value.contains('\\') || value.as_bytes().contains(&0) {
        return false;
    }
    let path = Path::new(value);
    !path.is_absolute()
        && path
            .components()
            .all(|component| matches!(component, Component::Normal(_)))
        && path.to_string_lossy() == value
}

fn valid_token(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 96
        && value.bytes().all(|byte| {
            byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-' | b'+' | b':')
        })
}

fn valid_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    const VALID: &str = concat!(
        "KFACEAUTH_MODEL_MANIFEST\t1\n",
        "id\tpath\tsize\tsha256\trole\tbackend\tlicense\tprovenance\n",
        "fake\tfiles/fake.cfg\t2\t",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "\ttest-config\tfake-deterministic\tGPL-3.0-or-later\trepo-authored\n",
    );
    static TEST_DIRECTORY_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn verified_test_root() -> PathBuf {
        let sequence = TEST_DIRECTORY_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "kfaceauth-model-inventory-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(root.join("files")).unwrap();
        let artifact = b"ok";
        fs::write(root.join("files/fake.cfg"), artifact).unwrap();
        let manifest = format!(
            "{MANIFEST_MAGIC}\n{MANIFEST_HEADER}\n\
             fake\tfiles/fake.cfg\t{}\t{}\ttest-config\tfake-deterministic\tGPL-3.0-or-later\trepo-authored\n",
            artifact.len(),
            digest_hex(artifact)
        );
        fs::write(root.join(MANIFEST_FILE), manifest).unwrap();
        root
    }

    #[test]
    fn parses_closed_schema() {
        let manifest = ModelManifest::parse(VALID).unwrap();
        assert_eq!(manifest.entries().len(), 1);
        assert_eq!(manifest.entries()[0].path, Path::new("files/fake.cfg"));
    }

    #[test]
    fn rejects_unknown_columns_and_unsafe_paths() {
        assert!(ModelManifest::parse(&VALID.replace("id\tpath", "id\textra\tpath")).is_err());
        assert!(ModelManifest::parse(&VALID.replace("files/fake.cfg", "../fake.cfg")).is_err());
    }

    #[test]
    fn rejects_duplicate_and_unsorted_paths() {
        let duplicate = format!("{VALID}{}", VALID.lines().nth(2).unwrap()) + "\n";
        assert!(ModelManifest::parse(&duplicate).is_err());
        let unsorted = VALID.replace("fake\tfiles/fake.cfg", "fake\tz/fake.cfg")
            + &VALID
                .lines()
                .nth(2)
                .unwrap()
                .replace("fake\tfiles/fake.cfg", "other\ta/fake.cfg")
            + "\n";
        assert!(ModelManifest::parse(&unsorted).is_err());
    }

    #[test]
    fn complete_inventory_rejects_modified_missing_and_unlisted_files() {
        let root = verified_test_root();
        assert!(verify_model_root(&root).is_ok());

        fs::write(root.join("files/fake.cfg"), b"no").unwrap();
        assert!(matches!(
            verify_model_root(&root),
            Err(ModelError::ArtifactDigestMismatch)
        ));
        fs::write(root.join("files/fake.cfg"), b"ok").unwrap();

        fs::write(root.join("files/unlisted.bin"), b"x").unwrap();
        assert!(matches!(
            verify_model_root(&root),
            Err(ModelError::InventoryMismatch)
        ));
        fs::remove_file(root.join("files/unlisted.bin")).unwrap();
        fs::remove_file(root.join("files/fake.cfg")).unwrap();
        assert!(matches!(
            verify_model_root(&root),
            Err(ModelError::InventoryMismatch)
        ));
        fs::remove_dir_all(root).unwrap();
    }
}
