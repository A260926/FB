use super::*;
use crate::{context::Tree,vm::VmShape};
#[test]
fn cpu_baseline() {
    let patches = CELL_TO_VERT_TO_EDGES[0x81];
    assert_eq!(patches.len(), 2);
    assert_eq!(patches[0].len(), 3);
    assert_eq!(patches[1].len(), 3);
    println!("PARENT_TABLE_OK mask=0x81 pieces={} edges={patches:?}", patches.len());
    let (x,y,z)=Tree::axes();
    let shape=VmShape::from((x.square()+y.square()+z.square()).sqrt()-0.6);
    let settings=Settings {depth:4,threads:None,world_to_model:nalgebra::Matrix4::identity(),..Default::default()};
    let o=Octree::build(&shape,&settings).unwrap();
    let mesh=o.walk_dual();
    assert!(!mesh.vertices.is_empty() && !mesh.triangles.is_empty());
    let mut edges=std::collections::BTreeMap::<(usize,usize),i32>::new();
    let mut counts=std::collections::BTreeMap::<(usize,usize),usize>::new();
    for t in &mesh.triangles {
        for (a,b) in [(t.x,t.y),(t.y,t.z),(t.z,t.x)] {
            assert!(a<mesh.vertices.len() && b<mesh.vertices.len() && a!=b);
            let k=(a.min(b),a.max(b));
            *edges.entry(k).or_default()+=if a<b {1}else{-1};
            *counts.entry(k).or_default()+=1;
        }
    }
    assert!(edges.values().all(|&n|n==0));
    assert!(counts.values().all(|&n|n==2));
    assert!(mesh.vertices.iter().all(|v|v.iter().all(|f|f.is_finite())));
    println!("CPU_BASELINE_OK vertices={} triangles={} closed=true",mesh.vertices.len(),mesh.triangles.len());
}
#[test]
fn two_plane_fixture() {
    let (x,y,z)=Tree::axes();
    let s=(x+y+z+3.0)*0.5;
    let shape=VmShape::from((s.clone()-0.75).min(2.25-s));
    let settings=Settings{depth:1,threads:None,world_to_model:nalgebra::Matrix4::identity(),..Default::default()};
    let o=Octree::build(&shape,&settings).unwrap();
    let Cell::Branch{index}=o.root else {panic!("legacy root must remain split")};
    let mut pieces=0;
    for (i,c) in o.cells[index].iter().enumerate() {
        let Cell::Leaf(leaf)=c else {panic!("child must intersect a plane")};
        assert_eq!(CELL_TO_VERT_TO_EDGES[leaf.mask.index()].len(),1);
        let v=o.verts[leaf.index].pos;
        let sum=(v.x+v.y+v.z+3.0)*0.5;
        let h=if i.count_ones()<=1 {0.75}else{2.25};
        assert!((sum-h).abs()<1e-5,"child {i}: {sum} != {h}");
        pieces+=1;
    }
    assert_eq!(pieces,8);
    println!("TWO_PLANE_BASELINE_OK legacy_children=8 parent_pieces=2 planes=0.75,2.25");
}
