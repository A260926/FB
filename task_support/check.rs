pub struct Check { count: usize }
impl Check { pub fn check(&mut self, condition: bool, message: &str) {
    self.count+=1; assert!(condition,"{message}");
}}
pub fn run(name: &str, body: impl FnOnce(&mut Check)) {
    let mut c=Check{count:0};body(&mut c);assert!(c.count>0,"zero assertions");
    println!("TASK_CASE {name} assertions={}",c.count);
}
