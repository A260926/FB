use super::*;
pub struct Check { count: usize, failed: bool }
impl Check {
    pub fn check(&mut self, condition: bool, message: &str) {
        if !condition { self.failed=true; }
        assert!(condition,"{message}");
        self.count+=1;
    }
}
// Implemented by the contributor; tests run inside mesh::octree::task_target
// and may access existing private octree data with `super` imports.
include!(concat!(env!("CARGO_MANIFEST_DIR"), "/../task_support/multipatch_tests.rs"));
#[test]
fn registered_multipatch_suite() {
    let required=["topology","qef","seams","compat"];
    assert!(!CASES.is_empty(),"no cases registered");
    let mut groups=std::collections::BTreeMap::<&str,usize>::new();
    let mut names=std::collections::BTreeSet::new();
    let mut total=0;
    for &(group,name,run) in CASES {
        assert!(required.contains(&group),"unknown group");
        assert!(names.insert((group,name)),"duplicate case");
        let mut c=Check{count:0,failed:false};run(&mut c);
        assert!(!c.failed,"a failed check was caught by the callback");
        assert!(c.count>0,"case contains no checks");
        *groups.entry(group).or_default()+=c.count;total+=c.count;
        println!("TASK_CASE_OK group={group} name={name} checks={}",c.count);
    }
    assert!(required.iter().all(|g|groups.contains_key(g)),"missing group");
    println!("TASK_TARGET_OK cases={} checks={} groups=4",CASES.len(),total);
}
