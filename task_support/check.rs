// Assertion accounting for the four integration groups.
pub struct Check { name: &'static str, count: usize }
impl Check {
 pub fn check(&mut self, ok: bool, message: &str) {self.count+=1;assert!(ok,"{}: {}",self.name,message);}
}
pub fn run(name:&'static str,body:impl FnOnce(&mut Check)){
 let mut c=Check{name,count:0};body(&mut c);assert!(c.count>0,"empty group {}",name);
 println!("TASK_GROUP_DONE {} checks={}",name,c.count);
}
