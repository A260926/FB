//! Assertion accounting for the three named regression groups.
use std::sync::atomic::{AtomicUsize,Ordering};
static CHECKS: AtomicUsize=AtomicUsize::new(0);
pub fn check(ok:bool) { CHECKS.fetch_add(1,Ordering::SeqCst);assert!(ok,"task content assertion failed"); }
pub fn run(name:&str,body:impl FnOnce()) {
    let start=CHECKS.load(Ordering::SeqCst);body();
    let count=CHECKS.load(Ordering::SeqCst)-start;
    assert!(count>0,"no content assertions in group {name}");
    println!("TASK_GROUP_OK {name} checks={count}");
}
