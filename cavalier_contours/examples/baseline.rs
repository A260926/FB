use cavalier_contours::{
    pline_closed,
    polyline::PlineSource,
    shape_algorithms::{Shape, ShapeOffsetOptions},
};
fn main() {
    let outer = pline_closed![(-3.0, 0.0, 1.0), (3.0, 0.0, 1.0)];
    let hole = pline_closed![(-1.0, 0.0, -1.0), (1.0, 0.0, -1.0)];
    let shape = Shape::from_plines([outer, hole]);
    let out = shape.parallel_offset(0.25, &ShapeOffsetOptions::default());
    assert_eq!(out.ccw_plines.len(), 1);
    assert_eq!(out.cw_plines.len(), 1);
    let area: f64 = out.ccw_plines.iter().chain(out.cw_plines.iter())
        .map(|p| p.polyline.area()).sum();
    assert!((area - 6.0 * std::f64::consts::PI).abs() < 1e-9);
    let outer_ext = out.ccw_plines[0].polyline.extents().unwrap();
    let inner_ext = out.cw_plines[0].polyline.extents().unwrap();
    assert!((outer_ext.max_x - 2.75).abs() < 1e-12);
    assert!((inner_ext.max_x - 1.25).abs() < 1e-12);
    println!("CAVALIER_BASELINE_OK annulus=1 hole=1 area=6pi");
}
