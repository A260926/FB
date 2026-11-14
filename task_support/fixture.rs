//! Small, legal, non-executable i386 Mach-O object. All data are synthetic.
fn word(v: &mut Vec<u8>, n: u32) { v.extend_from_slice(&n.to_le_bytes()); }
fn name(v: &mut Vec<u8>, s: &str) { assert!(s.len() <= 16); let n = v.len(); v.extend_from_slice(s.as_bytes()); v.resize(n+16,0); }
pub fn basic_object() -> Vec<u8> {
    let mut v=Vec::new();
    for n in [0xfeedface,7,3,1,1,260,0] {word(&mut v,n);}
    word(&mut v,1);word(&mut v,260);name(&mut v,"__TEST");
    for n in [0x1000,0x3000,288,48,7,7,3,0] {word(&mut v,n);}
    for (s,a,o,r,c,f) in [("__text",0x1000,288,0,0,0),("__data",0x2000,304,0,0,0),("__debug_info",0x3000,320,336,2,0x02000000)] {
        name(&mut v,s);name(&mut v,"__TEST");
        for n in [a,16,o,2,r,c,f,0,0] {word(&mut v,n);}
    }
    assert_eq!(v.len(),288);v.resize(336,0);
    v[320..324].copy_from_slice(&0x1009u32.to_le_bytes());
    v[324..328].copy_from_slice(&7u32.to_le_bytes());
    // Scattered vanilla: old symbol value 0x1004, field contains symbol+5.
    word(&mut v,0xa0000000);word(&mut v,0x1004);
    // Plain local vanilla, section ordinal 1, width 32.
    word(&mut v,4);word(&mut v,0x04000001);
    v
}
