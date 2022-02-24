// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub use crate::render::generic::{BlendMode, Fill, FillRule, Gradient, GradientType, Order, Style};
use crate::{
    color::Color,
    render::generic::{
        self, mold::*, spinel::*, Composition as _, Context as _, PathBuilder as _, Raster as _,
        RasterBuilder as _,
    },
    Point, ViewAssistantContext,
};
use anyhow::Error;
use euclid::{
    default::{Point2D, Rect, Size2D, Transform2D, Vector2D},
    point2, size2,
};
use fuchsia_framebuffer::PixelFormat;
use std::{io::Read, mem, ops::Add, u32};
/// Rendering context and API start point.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct Image {
    inner: ImageInner,
}
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
enum ImageInner {
    Mold(MoldImage),
    Spinel(SpinelImage),
}
/// Rectangular copy region.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CopyRegion {
    /// Top-left origin of source rectangle.
    pub src_offset: Point2D<u32>,
    /// Top-left origin of destination rectangle.
    pub dst_offset: Point2D<u32>,
    /// Size of both source and destination rectangles.
    pub extent: Size2D<u32>,
}
/// Rendering extensions.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RenderExt {
    /// Clears render image before rendering.
    pub pre_clear: Option<PreClear>,
    /// Copies from source image to render image before rendering.
    pub pre_copy: Option<PreCopy>,
    /// Copies from render image to destination image after rendering.
    pub post_copy: Option<PostCopy>,
}
/// Pre-render image clear.
#[derive(Clone, Debug, PartialEq)]
pub struct PreClear {
    /// Clear color.
    pub color: Color,
}
/// Pre-render image copy.
#[derive(Clone, Debug, PartialEq)]
pub struct PreCopy {
    /// Source image to copy from.
    pub image: Image,
    /// Copy region properties.
    pub copy_region: CopyRegion,
}
/// Post-render image copy.
#[derive(Clone, Debug, PartialEq)]
pub struct PostCopy {
    /// Destination image to copy to. Must be different from render image.
    pub image: Image,
    /// Copy region properties.
    pub copy_region: CopyRegion,
}
/// Rendering context and API start point.
#[derive(Debug)]
pub struct Context {
    pub inner: ContextInner,
}
#[derive(Debug)]
pub enum ContextInner {
    Mold(MoldContext),
    Spinel(SpinelContext),
}
impl Context {
    /// Returns the context's pixel format.
    pub fn pixel_format(&self) -> PixelFormat {
        match &self.inner {
            ContextInner::Mold(context) => context.pixel_format(),
            ContextInner::Spinel(context) => context.pixel_format(),
        }
    }
    /// Optionally returns a `PathBuilder`. May return `None` of old builder is still alive.
    pub fn path_builder(&self) -> Option<PathBuilder> {
        match &self.inner {
            ContextInner::Mold(context) => context
                .path_builder()
                .map(|path_builder| PathBuilder { inner: PathBuilderInner::Mold(path_builder) }),
            ContextInner::Spinel(context) => context
                .path_builder()
                .map(|path_builder| PathBuilder { inner: PathBuilderInner::Spinel(path_builder) }),
        }
    }
    /// Optionally returns a `RasterBuilder`. May return `None` of old builder is still alive.
    pub fn raster_builder(&self) -> Option<RasterBuilder> {
        match &self.inner {
            ContextInner::Mold(context) => context.raster_builder().map(|raster_builder| {
                RasterBuilder { inner: RasterBuilderInner::Mold(raster_builder) }
            }),
            ContextInner::Spinel(context) => context.raster_builder().map(|raster_builder| {
                RasterBuilder { inner: RasterBuilderInner::Spinel(raster_builder) }
            }),
        }
    }
    /// Creates a new image with `size`.
    pub fn new_image(&mut self, size: Size2D<u32>) -> Image {
        match &mut self.inner {
            ContextInner::Mold(ref mut context) => {
                Image { inner: ImageInner::Mold(context.new_image(size)) }
            }
            ContextInner::Spinel(ref mut context) => {
                Image { inner: ImageInner::Spinel(context.new_image(size)) }
            }
        }
    }
    /// Creates a new image from PNG `reader`.
    pub fn new_image_from_png<R: Read>(
        &mut self,
        reader: &mut png::Reader<R>,
    ) -> Result<Image, Error> {
        Ok(Image {
            inner: match &mut self.inner {
                ContextInner::Mold(ref mut context) => {
                    ImageInner::Mold(context.new_image_from_png(reader)?)
                }
                ContextInner::Spinel(ref mut context) => {
                    ImageInner::Spinel(context.new_image_from_png(reader)?)
                }
            },
        })
    }
    /// Returns the image at `image_index`.
    pub fn get_image(&mut self, image_index: u32) -> Image {
        match &mut self.inner {
            ContextInner::Mold(ref mut render_context) => {
                Image { inner: ImageInner::Mold(render_context.get_image(image_index)) }
            }
            ContextInner::Spinel(ref mut render_context) => {
                Image { inner: ImageInner::Spinel(render_context.get_image(image_index)) }
            }
        }
    }
    /// Returns the `context`'s current image.
    pub fn get_current_image(&mut self, context: &ViewAssistantContext) -> Image {
        match &mut self.inner {
            ContextInner::Mold(ref mut render_context) => {
                Image { inner: ImageInner::Mold(render_context.get_current_image(context)) }
            }
            ContextInner::Spinel(ref mut render_context) => {
                Image { inner: ImageInner::Spinel(render_context.get_current_image(context)) }
            }
        }
    }

    /// Renders the composition with an optional clip to the image.
    pub fn render(
        &mut self,
        composition: &mut Composition,
        clip: Option<Rect<u32>>,
        image: Image,
        ext: &RenderExt,
    ) {
        self.render_with_clip(
            composition,
            clip.unwrap_or_else(|| {
                Rect::new(point2(u32::MIN, u32::MIN), size2(u32::MAX, u32::MAX))
            }),
            image,
            ext,
        );
    }
    /// Renders the composition with a clip to the image.
    pub fn render_with_clip(
        &mut self,
        composition: &mut Composition,
        clip: Rect<u32>,
        image: Image,
        ext: &RenderExt,
    ) {
        let background_color = composition.background_color;

        match &mut self.inner {
            ContextInner::Mold(ref mut context) => {
                if let ImageInner::Mold(image) = image.inner {
                    let ext = generic::RenderExt {
                        pre_clear: ext
                            .pre_clear
                            .clone()
                            .map(|pre_clear| generic::PreClear { color: pre_clear.color }),
                        pre_copy: ext.pre_copy.clone().map(|pre_copy| generic::PreCopy {
                            image: if let ImageInner::Mold(image) = pre_copy.image.inner {
                                image
                            } else {
                                panic!("mismatched backends");
                            },
                            copy_region: generic::CopyRegion {
                                src_offset: pre_copy.copy_region.src_offset,
                                dst_offset: pre_copy.copy_region.dst_offset,
                                extent: pre_copy.copy_region.extent,
                            },
                        }),
                        post_copy: ext.post_copy.clone().map(|post_copy| generic::PostCopy {
                            image: if let ImageInner::Mold(image) = post_copy.image.inner {
                                image
                            } else {
                                panic!("mismatched backends");
                            },
                            copy_region: generic::CopyRegion {
                                src_offset: post_copy.copy_region.src_offset,
                                dst_offset: post_copy.copy_region.dst_offset,
                                extent: post_copy.copy_region.extent,
                            },
                        }),
                    };
                    composition.with_inner_composition(|inner| {
                        let mut composition = match inner {
                            CompositionInner::Mold(composition) => composition,
                            CompositionInner::Empty => MoldComposition::new(background_color),
                            _ => panic!("mismatched backends"),
                        };
                        context.render_with_clip(&mut composition, clip, image, &ext);
                        CompositionInner::Mold(composition)
                    });
                } else {
                    panic!("mismatched backends");
                }
            }
            ContextInner::Spinel(ref mut context) => {
                if let ImageInner::Spinel(image) = image.inner {
                    let ext = generic::RenderExt {
                        pre_clear: ext
                            .pre_clear
                            .clone()
                            .map(|pre_clear| generic::PreClear { color: pre_clear.color }),
                        pre_copy: ext.pre_copy.clone().map(|pre_copy| generic::PreCopy {
                            image: if let ImageInner::Spinel(image) = pre_copy.image.inner {
                                image
                            } else {
                                panic!("mismatched backends");
                            },
                            copy_region: generic::CopyRegion {
                                src_offset: pre_copy.copy_region.src_offset,
                                dst_offset: pre_copy.copy_region.dst_offset,
                                extent: pre_copy.copy_region.extent,
                            },
                        }),
                        post_copy: ext.post_copy.clone().map(|post_copy| generic::PostCopy {
                            image: if let ImageInner::Spinel(image) = post_copy.image.inner {
                                image
                            } else {
                                panic!("mismatched backends");
                            },
                            copy_region: generic::CopyRegion {
                                src_offset: post_copy.copy_region.src_offset,
                                dst_offset: post_copy.copy_region.dst_offset,
                                extent: post_copy.copy_region.extent,
                            },
                        }),
                    };
                    composition.with_inner_composition(|inner| {
                        let mut composition = match inner {
                            CompositionInner::Spinel(composition) => composition,
                            // All Spinel rendering is performed in a linear color space, and is
                            // transformed to sRGB as it is written to the output image (possibly
                            // via an additional styling opcode).  Therefore, we provide a linear
                            // background color.
                            CompositionInner::Empty => SpinelComposition::new(background_color),
                            _ => panic!("mismatched backends"),
                        };
                        context.render_with_clip(&mut composition, clip, image, &ext);
                        CompositionInner::Spinel(composition)
                    });
                } else {
                    panic!("mismatched backends");
                }
            }
        }
    }
}
/// Closed path built by a `PathBuilder`.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Path {
    inner: PathInner,
}
#[derive(Clone, Debug, Eq, PartialEq)]
enum PathInner {
    Mold(MoldPath),
    Spinel(SpinelPath),
}
/// Builds one closed path.
#[derive(Debug)]
pub struct PathBuilder {
    inner: PathBuilderInner,
}
#[derive(Debug)]
enum PathBuilderInner {
    Mold(MoldPathBuilder),
    Spinel(SpinelPathBuilder),
}
impl PathBuilder {
    /// Move end-point to.
    pub fn move_to(&mut self, point: Point) -> &mut Self {
        match &mut self.inner {
            PathBuilderInner::Mold(ref mut path_builder) => {
                path_builder.move_to(point);
            }
            PathBuilderInner::Spinel(ref mut path_builder) => {
                path_builder.move_to(point);
            }
        }
        self
    }
    /// Create line from end-point to point and update end-point.
    pub fn line_to(&mut self, point: Point) -> &mut Self {
        match &mut self.inner {
            PathBuilderInner::Mold(ref mut path_builder) => {
                path_builder.line_to(point);
            }
            PathBuilderInner::Spinel(ref mut path_builder) => {
                path_builder.line_to(point);
            }
        }
        self
    }
    /// Create quadratic Bézier from end-point to `p2` with `p1` as control point.
    pub fn quad_to(&mut self, p1: Point, p2: Point) -> &mut Self {
        match &mut self.inner {
            PathBuilderInner::Mold(ref mut path_builder) => {
                path_builder.quad_to(p1, p2);
            }
            PathBuilderInner::Spinel(ref mut path_builder) => {
                path_builder.quad_to(p1, p2);
            }
        }
        self
    }
    /// Create cubic Bézier from end-point to `p3` with `p1` and `p2` as control points.
    pub fn cubic_to(&mut self, p1: Point, p2: Point, p3: Point) -> &mut Self {
        match &mut self.inner {
            PathBuilderInner::Mold(ref mut path_builder) => {
                path_builder.cubic_to(p1, p2, p3);
            }
            PathBuilderInner::Spinel(ref mut path_builder) => {
                path_builder.cubic_to(p1, p2, p3);
            }
        }
        self
    }
    /// Create rational quadratic Bézier from end-point to `p2` with `p1` as control point
    /// and `w` as its weight.
    pub fn rat_quad_to(&mut self, p1: Point, p2: Point, w: f32) -> &mut Self {
        match &mut self.inner {
            PathBuilderInner::Mold(ref mut path_builder) => {
                path_builder.rat_quad_to(p1, p2, w);
            }
            PathBuilderInner::Spinel(ref mut path_builder) => {
                path_builder.rat_quad_to(p1, p2, w);
            }
        }
        self
    }
    /// Create rational cubic Bézier from end-point to `p3` with `p1` and `p2` as control
    /// points, and `w1` and `w2` their weights.
    pub fn rat_cubic_to(&mut self, p1: Point, p2: Point, p3: Point, w1: f32, w2: f32) -> &mut Self {
        match &mut self.inner {
            PathBuilderInner::Mold(ref mut path_builder) => {
                path_builder.rat_cubic_to(p1, p2, p3, w1, w2);
            }
            PathBuilderInner::Spinel(ref mut path_builder) => {
                path_builder.rat_cubic_to(p1, p2, p3, w1, w2);
            }
        }
        self
    }
    /// Closes the path with a line if not yet closed and builds the path.
    ///
    /// Consumes the builder; another one can be requested from the `Context`.
    pub fn build(self) -> Path {
        match self.inner {
            PathBuilderInner::Mold(path_builder) => {
                Path { inner: PathInner::Mold(path_builder.build()) }
            }
            PathBuilderInner::Spinel(path_builder) => {
                Path { inner: PathInner::Spinel(path_builder.build()) }
            }
        }
    }
}
/// Raster built by a `RasterBuilder`.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Raster {
    inner: RasterInner,
}
#[derive(Clone, Debug, Eq, PartialEq)]
enum RasterInner {
    Mold(MoldRaster),
    Spinel(SpinelRaster),
}
impl Raster {
    /// Translate raster.
    pub fn translate(self, translation: Vector2D<i32>) -> Self {
        match self.inner {
            RasterInner::Mold(raster) => {
                Raster { inner: RasterInner::Mold(raster.translate(translation)) }
            }
            RasterInner::Spinel(raster) => {
                Raster { inner: RasterInner::Spinel(raster.translate(translation)) }
            }
        }
    }
}
impl Add for Raster {
    type Output = Self;
    fn add(self, other: Self) -> Self::Output {
        match self.inner {
            RasterInner::Mold(raster) => Raster {
                inner: if let RasterInner::Mold(other_raster) = other.inner {
                    RasterInner::Mold(raster + other_raster)
                } else {
                    panic!("mismatched backends");
                },
            },
            RasterInner::Spinel(raster) => Raster {
                inner: if let RasterInner::Spinel(other_raster) = other.inner {
                    RasterInner::Spinel(raster + other_raster)
                } else {
                    panic!("mismatched backends");
                },
            },
        }
    }
}
/// Builds one Raster.
#[derive(Debug)]
pub struct RasterBuilder {
    inner: RasterBuilderInner,
}
#[derive(Debug)]
enum RasterBuilderInner {
    Mold(MoldRasterBuilder),
    Spinel(SpinelRasterBuilder),
}
impl RasterBuilder {
    /// Add a path to the raster with optional transform.
    pub fn add(&mut self, path: &Path, transform: Option<&Transform2D<f32>>) -> &mut Self {
        self.add_with_transform(path, transform.unwrap_or(&Transform2D::identity()))
    }
    /// Add a path to the raster with transform.
    pub fn add_with_transform(&mut self, path: &Path, transform: &Transform2D<f32>) -> &mut Self {
        match &mut self.inner {
            RasterBuilderInner::Mold(ref mut raster_builder) => {
                if let PathInner::Mold(path) = &path.inner {
                    raster_builder.add_with_transform(path, transform);
                } else {
                    panic!("mismatched backends");
                }
            }
            RasterBuilderInner::Spinel(ref mut raster_builder) => {
                if let PathInner::Spinel(path) = &path.inner {
                    raster_builder.add_with_transform(path, transform);
                } else {
                    panic!("mismatched backends");
                }
            }
        }
        self
    }
    /// Builds the raster.
    ///
    /// Consumes the builder; another one can be requested from the `Context`.
    pub fn build(self) -> Raster {
        match self.inner {
            RasterBuilderInner::Mold(raster_builder) => {
                Raster { inner: RasterInner::Mold(raster_builder.build()) }
            }
            RasterBuilderInner::Spinel(raster_builder) => {
                Raster { inner: RasterInner::Spinel(raster_builder.build()) }
            }
        }
    }
}

#[derive(Clone, Debug)]
pub struct Layer {
    /// Layer raster.
    pub raster: Raster,
    /// Layer clip.
    pub clip: Option<Raster>,
    /// Layer style.
    pub style: Style, // Will also contain txty when available.
}
#[derive(Debug)]
/// A group of ordered layers.
pub struct Composition {
    inner: CompositionInner,
    background_color: Color,
}
#[derive(Debug)]
enum CompositionInner {
    Mold(MoldComposition),
    Spinel(SpinelComposition),
    Empty,
}
impl Composition {
    /// Creates a composition of ordered layers where the layers with lower index appear on top.
    pub fn new(background_color: Color) -> Self {
        Self { inner: CompositionInner::Empty, background_color }
    }
    fn with_inner_composition(&mut self, mut f: impl FnMut(CompositionInner) -> CompositionInner) {
        let inner = f(mem::replace(&mut self.inner, CompositionInner::Empty));
        self.inner = inner;
    }
    /// Resets composition by removing all layers.
    pub fn clear(&mut self) {
        match &mut self.inner {
            CompositionInner::Mold(composition) => composition.clear(),
            CompositionInner::Spinel(composition) => composition.clear(),
            CompositionInner::Empty => (),
        }
    }
    /// Insert layer into composition.
    pub fn insert(&mut self, order: Order, layer: Layer) {
        if let CompositionInner::Empty = self.inner {
            match layer {
                Layer { raster: Raster { inner: RasterInner::Mold(_) }, .. } => {
                    self.inner =
                        CompositionInner::Mold(MoldComposition::new(self.background_color));
                }
                Layer { raster: Raster { inner: RasterInner::Spinel(_) }, .. } => {
                    self.inner =
                        CompositionInner::Spinel(SpinelComposition::new(self.background_color));
                }
            }
        }
        match &mut self.inner {
            CompositionInner::Mold(composition) => composition.insert(
                order,
                generic::Layer {
                    raster: if let RasterInner::Mold(raster) = layer.raster.inner {
                        raster
                    } else {
                        panic!("mismatched backends");
                    },
                    clip: layer.clip.map(|clip| {
                        if let RasterInner::Mold(clip) = clip.inner {
                            clip
                        } else {
                            panic!("mismatched backends");
                        }
                    }),
                    style: layer.style,
                },
            ),
            CompositionInner::Spinel(composition) => composition.insert(
                order,
                generic::Layer {
                    raster: if let RasterInner::Spinel(raster) = layer.raster.inner {
                        raster
                    } else {
                        panic!("mismatched backends");
                    },
                    clip: layer.clip.map(|clip| {
                        if let RasterInner::Spinel(clip) = clip.inner {
                            clip
                        } else {
                            panic!("mismatched backends");
                        }
                    }),
                    style: layer.style,
                },
            ),
            _ => unreachable!(),
        }
    }
    /// Remove layer from composition.
    pub fn remove(&mut self, order: Order) {
        match &mut self.inner {
            CompositionInner::Mold(composition) => composition.remove(order),
            CompositionInner::Spinel(composition) => composition.remove(order),
            _ => (),
        }
    }
}
