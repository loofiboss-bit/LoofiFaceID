// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::fmt;

use zeroize::{Zeroize, ZeroizeOnDrop};

pub const DETECTOR_MODEL_ID: &str = "yunet-2023mar-v1";
pub const EMBEDDING_MODEL_ID: &str = "sface-2021dec-fp32-v1";
pub const EMBEDDING_FORMAT_ID: &str = "sface-f32-le-128-l2-v1";
pub const SFACE_MODEL_SHA256: &str =
    "0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79";
pub const NORMALIZATION_VERSION: u16 = 1;
pub const EMBEDDING_DIMENSION: usize = 128;

const MAXIMUM_RAW_ABSOLUTE_VALUE: f32 = 32.0;
const MINIMUM_NORM: f64 = 1.0e-12;
const NORMALIZED_NORM_TOLERANCE: f64 = 1.0e-5;

#[derive(Zeroize, ZeroizeOnDrop)]
pub struct NormalizedEmbedding {
    values: [f32; EMBEDDING_DIMENSION],
}

impl NormalizedEmbedding {
    /// Validates and deterministically L2-normalizes a raw `SFace` feature.
    ///
    /// # Errors
    ///
    /// Rejects non-finite, out-of-bound, zero-norm, or malformed features.
    #[allow(clippy::cast_possible_truncation)]
    pub fn from_raw(mut values: [f32; EMBEDDING_DIMENSION]) -> Result<Self, EmbeddingError> {
        if values
            .iter()
            .any(|value| !value.is_finite() || value.abs() > MAXIMUM_RAW_ABSOLUTE_VALUE)
        {
            values.zeroize();
            return Err(EmbeddingError);
        }
        let norm_squared = values
            .iter()
            .map(|value| f64::from(*value) * f64::from(*value))
            .sum::<f64>();
        if !norm_squared.is_finite() || norm_squared <= MINIMUM_NORM {
            values.zeroize();
            return Err(EmbeddingError);
        }
        let norm = norm_squared.sqrt();
        for value in &mut values {
            *value = (f64::from(*value) / norm) as f32;
        }
        Self::from_normalized(values)
    }

    /// Reconstructs a normalized feature from authenticated vault plaintext.
    ///
    /// # Errors
    ///
    /// Rejects data that is not finite, bounded, and unit-normalized.
    pub fn from_normalized(mut values: [f32; EMBEDDING_DIMENSION]) -> Result<Self, EmbeddingError> {
        if values
            .iter()
            .any(|value| !value.is_finite() || value.abs() > 1.0)
        {
            values.zeroize();
            return Err(EmbeddingError);
        }
        let norm = values
            .iter()
            .map(|value| f64::from(*value) * f64::from(*value))
            .sum::<f64>()
            .sqrt();
        if !norm.is_finite() || (norm - 1.0).abs() > NORMALIZED_NORM_TOLERANCE {
            values.zeroize();
            return Err(EmbeddingError);
        }
        Ok(Self { values })
    }

    /// Returns sensitive feature values to trusted Rust identity/vault code.
    #[must_use]
    pub fn sensitive_values(&self) -> &[f32; EMBEDDING_DIMENSION] {
        &self.values
    }
}

impl Clone for NormalizedEmbedding {
    fn clone(&self) -> Self {
        Self {
            values: self.values,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EmbeddingError;

impl fmt::Display for EmbeddingError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("embedding violates the normalized representation contract")
    }
}

impl std::error::Error for EmbeddingError {}

/// Computes deterministic cosine similarity for validated unit embeddings.
#[must_use]
pub fn cosine_similarity(left: &NormalizedEmbedding, right: &NormalizedEmbedding) -> f64 {
    left.sensitive_values()
        .iter()
        .zip(right.sensitive_values())
        .map(|(left, right)| f64::from(*left) * f64::from(*right))
        .sum::<f64>()
        .clamp(-1.0, 1.0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn basis(index: usize) -> NormalizedEmbedding {
        let mut values = [0.0_f32; EMBEDDING_DIMENSION];
        values[index] = 1.0;
        NormalizedEmbedding::from_normalized(values).unwrap()
    }

    #[test]
    fn normalization_rejects_malformed_values_and_zero_norm() {
        assert_eq!(
            NormalizedEmbedding::from_raw([0.0; EMBEDDING_DIMENSION]).err(),
            Some(EmbeddingError)
        );
        let mut non_finite = [0.0; EMBEDDING_DIMENSION];
        non_finite[0] = f32::NAN;
        assert_eq!(
            NormalizedEmbedding::from_raw(non_finite).err(),
            Some(EmbeddingError)
        );
        let mut out_of_bounds = [0.0; EMBEDDING_DIMENSION];
        out_of_bounds[0] = 33.0;
        assert_eq!(
            NormalizedEmbedding::from_raw(out_of_bounds).err(),
            Some(EmbeddingError)
        );
    }

    #[test]
    fn normalization_and_cosine_are_deterministic() {
        let mut raw = [0.0; EMBEDDING_DIMENSION];
        raw[0] = 3.0;
        raw[1] = 4.0;
        let normalized = NormalizedEmbedding::from_raw(raw).unwrap();
        assert!((normalized.sensitive_values()[0] - 0.6).abs() < 1.0e-6);
        assert!((normalized.sensitive_values()[1] - 0.8).abs() < 1.0e-6);
        assert!((cosine_similarity(&normalized, &normalized) - 1.0).abs() < 1.0e-6);
        assert!(cosine_similarity(&basis(0), &basis(1)).abs() < f64::EPSILON);
    }
}
