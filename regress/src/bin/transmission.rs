//! Regression for direct-light transmission through transparent surfaces.
//!
//! Verifies that a backlit transparent Plane/Triangle receives direct light on
//! both sides, that the geometric orientation survives transforms, and that the
//! opaque-plane shading behavior and shadow visibility are unchanged.

use std::path::{Path, PathBuf};

use rpt::glm;
use rpt::*;

const INDEX: f64 = 1.5;
const ROUGHNESS: f64 = 0.3;
const INTENSITY: f64 = 0.001;
/// Linear color for a camera on the positive side of the geometric normal.
const FRONT_EXPECTED: f64 = 0.013581221811;
/// Linear color for a camera on the negative side of the geometric normal.
const BACK_EXPECTED: f64 = 0.030557749074;
const TOLERANCE: f64 = 1e-7;
const FOV: f64 = 1e-6;

fn white() -> glm::DVec3 {
    glm::vec3(1.0, 1.0, 1.0)
}

fn clear_material() -> Material {
    Material::clear(INDEX, ROUGHNESS)
}

fn camera_up(n: &glm::DVec3) -> glm::DVec3 {
    if n.x.abs() < 0.9 {
        glm::vec3(1.0, 0.0, 0.0)
    } else {
        glm::vec3(0.0, 1.0, 0.0)
    }
}

/// Render a 1x1 image with a tiny field of view, returning the linear color.
///
/// The tiny FOV keeps camera-jitter-induced direction changes negligible while
/// still exercising the public `Renderer` builder and path.
fn render_linear(scene: &Scene, camera: Camera) -> glm::DVec3 {
    let renderer = Renderer::new(scene, camera)
        .width(1)
        .height(1)
        .num_samples(1)
        .max_bounces(0);
    let mut color = glm::zero();
    renderer.iterative_render(1, |_, buffer| {
        color = buffer.linear_color(0, 0);
    });
    color
}

fn render_pixel(scene: &Scene, camera: Camera) -> [u8; 3] {
    Renderer::new(scene, camera)
        .width(1)
        .height(1)
        .num_samples(1)
        .max_bounces(0)
        .render()
        .get_pixel(0, 0)
        .0
}

fn assert_linear(label: &str, color: glm::DVec3, expected: f64) {
    println!(
        "{} linear=({:.12}, {:.12}, {:.12}) expected={:.12}",
        label, color.x, color.y, color.z, expected
    );
    for &value in &[color.x, color.y, color.z] {
        assert!(
            (value - expected).abs() <= TOLERANCE,
            "{}: got {:.12}, expected {:.12} (tolerance {:.1e})",
            label,
            value,
            expected,
            TOLERANCE
        );
    }
    assert!(
        color.x >= 0.0 && color.y >= 0.0 && color.z >= 0.0,
        "{}: produced negative radiance {:?}",
        label,
        color
    );
}

fn assert_pair_matches(label: &str, a: glm::DVec3, b: glm::DVec3) {
    println!(
        "{} match=({:.12}, {:.12}, {:.12}) vs ({:.12}, {:.12}, {:.12})",
        label, a.x, a.y, a.z, b.x, b.y, b.z
    );
    for &(x, y) in &[(a.x, b.x), (a.y, b.y), (a.z, b.z)] {
        assert!(
            (x - y).abs() <= TOLERANCE,
            "{}: {:.12} != {:.12}",
            label,
            x,
            y
        );
    }
}

/// Build a scene with one object and a directional light travelling along `dir`.
fn directional_scene(
    shape: Box<dyn Shape>,
    material: Material,
    dir: glm::DVec3,
    intensity: f64,
) -> Scene {
    let mut scene = Scene::new();
    scene.add(Object::new(shape).material(material));
    scene.add(Light::Directional(
        glm::vec3(intensity, intensity, intensity),
        dir,
    ));
    scene
}

/// Render the same shape from the positive and negative side of `n`.
///
/// In both cases the directional light comes from the side opposite the camera,
/// so the direct path is transmission. `make` constructs the shape twice because
/// the scene takes ownership of it.
fn render_transmission_pair<F>(make: F, n: glm::DVec3) -> (glm::DVec3, glm::DVec3)
where
    F: Fn() -> Box<dyn Shape>,
{
    let front = render_linear(
        &directional_scene(make(), clear_material(), n, INTENSITY),
        Camera::look_at(n, glm::zero(), camera_up(&n), FOV),
    );
    let back = render_linear(
        &directional_scene(make(), clear_material(), -n, INTENSITY),
        Camera::look_at(-n, glm::zero(), camera_up(&n), FOV),
    );
    (front, back)
}

fn big_triangle() -> Triangle {
    // Winding gives the +z geometric normal.
    Triangle::from_vertices(
        glm::vec3(-100.0, -100.0, 0.0),
        glm::vec3(100.0, -100.0, 0.0),
        glm::vec3(0.0, 100.0, 0.0),
    )
}

fn main() {
    let n = glm::vec3(0.0, 0.0, 1.0);

    // 1. The BSDF reference value quoted by the task.
    let bsdf = clear_material().bsdf(&n, &n, &-n);
    println!("TRANSMISSION_BSDF={:.12}", bsdf.x);
    assert!(
        (bsdf.x - 13.581221811).abs() <= TOLERANCE,
        "BSDF: got {:.12}",
        bsdf.x
    );

    // 2. Transparent Plane, front side and back side.
    let (plane_front, plane_back) =
        render_transmission_pair(|| -> Box<dyn Shape> { Box::new(plane(n, 0.0)) }, n);
    assert_linear("PLANE_FRONT", plane_front, FRONT_EXPECTED);
    assert_linear("PLANE_BACK", plane_back, BACK_EXPECTED);

    // 3. Transparent Triangle with the same geometric orientation.
    let (triangle_front, triangle_back) =
        render_transmission_pair(|| -> Box<dyn Shape> { Box::new(big_triangle()) }, n);
    assert_linear("TRIANGLE_FRONT", triangle_front, FRONT_EXPECTED);
    assert_linear("TRIANGLE_BACK", triangle_back, BACK_EXPECTED);
    assert_pair_matches("PLANE_TRIANGLE_FRONT", plane_front, triangle_front);
    assert_pair_matches("PLANE_TRIANGLE_BACK", plane_back, triangle_back);

    // 4. Rotated and non-uniformly positively scaled shapes, compared against
    //    the equivalent large triangle in the same plane and orientation.
    let scale = glm::vec3(2.0, 0.75, 1.5);
    let transformed_normal = glm::normalize(&glm::vec3(0.0, -(0.4_f64.sin()), 0.4_f64.cos()));

    let (tplane_front, tplane_back) = render_transmission_pair(
        || -> Box<dyn Shape> { Box::new(plane(n, 0.0).scale(&scale).rotate_x(0.4)) },
        transformed_normal,
    );
    let (ttri_front, ttri_back) = render_transmission_pair(
        || -> Box<dyn Shape> { Box::new(big_triangle().scale(&scale).rotate_x(0.4)) },
        transformed_normal,
    );
    assert_linear("TRANSFORMED_PLANE_FRONT", tplane_front, FRONT_EXPECTED);
    assert_linear("TRANSFORMED_PLANE_BACK", tplane_back, BACK_EXPECTED);
    assert_linear("TRANSFORMED_TRIANGLE_FRONT", ttri_front, FRONT_EXPECTED);
    assert_linear("TRANSFORMED_TRIANGLE_BACK", ttri_back, BACK_EXPECTED);
    assert_pair_matches("TRANSFORMED_FRONT", tplane_front, ttri_front);
    assert_pair_matches("TRANSFORMED_BACK", tplane_back, ttri_back);

    // 5. Opaque Plane: the existing viewer-facing behavior is preserved on both
    //    sides, and a backlit opaque surface stays black.
    let diffuse = Material::diffuse(white());
    let front_cam = Camera::look_at(n, glm::zero(), camera_up(&n), FOV);
    let back_cam = Camera::look_at(-n, glm::zero(), camera_up(&n), FOV);

    let opaque_front = render_pixel(
        &directional_scene(Box::new(plane(n, 0.0)), diffuse, -n, 1.0),
        front_cam,
    );
    let opaque_back = render_pixel(
        &directional_scene(Box::new(plane(n, 0.0)), diffuse, n, 1.0),
        back_cam,
    );
    let opaque_backlit = render_pixel(
        &directional_scene(Box::new(plane(n, 0.0)), diffuse, n, 1.0),
        front_cam,
    );
    println!("OPAQUE_FRONT_PIXEL {:?}", opaque_front);
    println!("OPAQUE_BACK_PIXEL {:?}", opaque_back);
    println!("OPAQUE_BACKLIT_PIXEL {:?}", opaque_backlit);
    assert_eq!(opaque_front, [149, 149, 149], "opaque front pixel");
    assert_eq!(opaque_back, [149, 149, 149], "opaque back pixel");
    assert_eq!(opaque_backlit, [0, 0, 0], "opaque backlit pixel");

    // 6. A real occluder behind the transparent surface still blocks the light.
    let mut occluded_scene = Scene::new();
    occluded_scene.add(Object::new(plane(n, 0.0)).material(clear_material()));
    occluded_scene.add(Object::new(plane(n, -2.0)).material(Material::diffuse(white())));
    occluded_scene.add(Light::Directional(
        glm::vec3(INTENSITY, INTENSITY, INTENSITY),
        n,
    ));
    let occluded = render_linear(&occluded_scene, front_cam);
    assert_linear("OCCLUDED_TRANSMISSION", occluded, 0.0);

    // 7. No light means no direct contribution.
    let mut unlit_scene = Scene::new();
    unlit_scene.add(Object::new(plane(n, 0.0)).material(clear_material()));
    let unlit = render_linear(&unlit_scene, front_cam);
    assert_linear("UNLIT_TRANSMISSION", unlit, 0.0);

    // 8. Light on the camera's own side only reflects; it is not transmitted.
    //    Expected value: specular = D * F * G / (4 (n.wo) (n.wi)) at normal
    //    incidence, with D = 1/(pi m^2), F = F0 = 0.04, G = 1.
    let reflect_expected =
        1.0 / (std::f64::consts::PI * ROUGHNESS * ROUGHNESS) * 0.04 / 4.0 * INTENSITY;
    let front_lit = render_linear(
        &directional_scene(Box::new(plane(n, 0.0)), clear_material(), -n, INTENSITY),
        front_cam,
    );
    assert_linear("FRONT_SIDE_REFLECTION", front_lit, reflect_expected);
    assert!(
        (front_lit.x - FRONT_EXPECTED).abs() > TOLERANCE,
        "front-side light must not be mistaken for transmission"
    );

    // 9. Point lights follow the same convention as directional lights.
    let mut point_scene = Scene::new();
    point_scene.add(Object::new(plane(n, 0.0)).material(clear_material()));
    point_scene.add(Light::Point(
        glm::vec3(INTENSITY, INTENSITY, INTENSITY),
        glm::vec3(0.0, 0.0, -1.0),
    ));
    let point_front = render_linear(&point_scene, front_cam);
    assert_linear("POINT_LIGHT_TRANSMISSION", point_front, FRONT_EXPECTED);

    // 10. Produce a small image through the real Renderer's byte output path.
    let output_path = output_path();
    write_png(&output_path);

    println!("TRANSMISSION_TARGET_OK");
}

fn output_path() -> PathBuf {
    std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| std::env::temp_dir().join("rpt-transmission.png"))
}

fn write_png(path: &Path) {
    let n = glm::vec3(0.0, 0.0, 1.0);
    let mut scene = Scene::new();
    scene.add(Object::new(plane(n, 0.0)).material(clear_material()));
    scene.add(Light::Directional(
        glm::vec3(INTENSITY, INTENSITY, INTENSITY),
        n,
    ));
    let camera = Camera::look_at(
        glm::vec3(0.0, 0.0, 1.0),
        glm::zero(),
        glm::vec3(1.0, 0.0, 0.0),
        0.6,
    );
    let image = Renderer::new(&scene, camera)
        .width(16)
        .height(16)
        .num_samples(4)
        .render();
    assert_eq!(image.dimensions(), (16, 16));
    for pixel in image.pixels() {
        assert!(
            pixel.0.iter().any(|&value| value > 0),
            "rendered image contains a black pixel"
        );
    }
    image
        .save(path)
        .expect("failed to save PNG regression image");
    let bytes = std::fs::metadata(path)
        .expect("missing PNG regression image")
        .len();
    assert!(bytes > 0, "PNG regression image is empty");
    println!("PNG_OUTPUT {} bytes={}", path.display(), bytes);
}
