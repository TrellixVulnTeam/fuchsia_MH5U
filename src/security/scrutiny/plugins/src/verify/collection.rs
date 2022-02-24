// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    cm_fidl_analyzer::component_model::ComponentModelForAnalyzer,
    scrutiny::prelude::*,
    serde::{Deserialize, Serialize},
    std::collections::HashSet,
    std::sync::Arc,
    uuid::Uuid,
};

#[derive(Deserialize, Serialize)]
pub struct V2ComponentModel {
    pub deps: HashSet<String>,
    // TODO(pesk): investigate options for partially serializing `component_model`.
    #[serde(skip)]
    pub component_model: Arc<ComponentModelForAnalyzer>,
    // TODO(pesk): replace this with a serializable error type.
    #[serde(skip)]
    pub errors: Vec<anyhow::Error>,
}

impl V2ComponentModel {
    pub fn new(
        deps: HashSet<String>,
        component_model: Arc<ComponentModelForAnalyzer>,
        errors: Vec<anyhow::Error>,
    ) -> Self {
        Self { deps, component_model, errors }
    }
}

impl DataCollection for V2ComponentModel {
    fn uuid() -> Uuid {
        Uuid::parse_str("3a44822e-9a41-3031-56db-158fb014e955").unwrap()
    }
    fn collection_name() -> String {
        "V2 Component Model Collection".to_string()
    }
    fn collection_description() -> String {
        "Contains a V2 component model based on the root.cm and all v2 manifests found in the build."
            .to_string()
    }
}
