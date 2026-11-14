// Legal synthetic ET_REL bytes only. These images are never executed.
#[derive(Clone, Default)]
pub struct Section {
    pub kind: u32, pub link: u32, pub info: u32, pub flags: u64,
    pub entsize: u64, pub data: Vec<u8>,
}
#[derive(Clone, Copy)]
pub struct Format { pub wide: bool, pub little: bool }
impl Format {
    pub fn word(self, out: &mut [u8], at: usize, value: u64, width: usize) {
        for i in 0..width { let shift = if self.little { i } else { width-1-i };
            out[at+i] = (value >> (shift*8)) as u8; }
    }
    pub fn words(self, values: &[u32]) -> Vec<u8> {
        let mut out = vec![0; values.len()*4];
        for (i,v) in values.iter().enumerate() { self.word(&mut out,i*4,*v as u64,4); } out
    }
    pub fn symbol(self, name: u32, raw_section: u16, value: u64) -> Vec<u8> {
        let mut b = vec![0; if self.wide {24} else {16}];
        self.word(&mut b,0,name as u64,4);
        if self.wide { b[4]=if name==0 && raw_section==0 && value==0 {0}else{0x10}; self.word(&mut b,6,raw_section as u64,2); self.word(&mut b,8,value,8); }
        else { self.word(&mut b,4,value,4); b[12]=if name==0 && raw_section==0 && value==0 {0}else{0x10}; self.word(&mut b,14,raw_section as u64,2); } b
    }
    pub fn relocation(self, symbol: u32, addend: Option<i64>) -> Vec<u8> {
        let w=if self.wide {8} else {4}; let mut b=vec![0;w*if addend.is_some(){3}else{2}];
        self.word(&mut b,0,4,w);
        self.word(&mut b,w,if self.wide {(symbol as u64)<<32|1} else {(symbol as u64)<<8|1},w);
        if let Some(a)=addend {self.word(&mut b,2*w,a as u64,w);} b
    }
    pub fn image(self, sections: &[Section]) -> Vec<u8> {
        assert!(!sections.is_empty()); assert!(sections[0].data.is_empty());
        let eh=if self.wide {64} else {52}; let sh=if self.wide {64} else {40};
        let table=eh; let mut b=vec![0;eh+sections.len()*sh];
        b[..4].copy_from_slice(b"\x7fELF"); b[4]=if self.wide{2}else{1}; b[5]=if self.little{1}else{2};b[6]=1;
        self.word(&mut b,16,1,2);self.word(&mut b,18,if self.wide{62}else{3},2);self.word(&mut b,20,1,4);
        if self.wide {self.word(&mut b,40,table as u64,8);self.word(&mut b,52,eh as u64,2);self.word(&mut b,58,sh as u64,2);self.word(&mut b,60,if sections.len()>=0xff00{0}else{sections.len()} as u64,2);}
        else {self.word(&mut b,32,table as u64,4);self.word(&mut b,40,eh as u64,2);self.word(&mut b,46,sh as u64,2);self.word(&mut b,48,if sections.len()>=0xff00{0}else{sections.len()} as u64,2);}
        self.word(&mut b,if self.wide{62}else{50},2,2);
        for (i,s) in sections.iter().enumerate() {
            let at=table+i*sh; let off=if i==0{0}else{b.len()}; b.extend_from_slice(&s.data);
            self.word(&mut b,at+4,s.kind as u64,4);
            let len=if i==0 && sections.len()>=0xff00 {sections.len()} else {s.data.len()};
            if self.wide {self.word(&mut b,at+8,s.flags,8);self.word(&mut b,at+24,off as u64,8);self.word(&mut b,at+32,len as u64,8);self.word(&mut b,at+40,s.link as u64,4);self.word(&mut b,at+44,s.info as u64,4);self.word(&mut b,at+48,if i==0{0}else{1},8);self.word(&mut b,at+56,s.entsize,8);}
            else {self.word(&mut b,at+8,s.flags,4);self.word(&mut b,at+16,off as u64,4);self.word(&mut b,at+20,len as u64,4);self.word(&mut b,at+24,s.link as u64,4);self.word(&mut b,at+28,s.info as u64,4);self.word(&mut b,at+32,if i==0{0}else{1},4);self.word(&mut b,at+36,s.entsize,4);}
        } b
    }
}
pub fn ordinary(f: Format) -> Vec<Section> {
    vec![Section::default(),Section{kind:1,data:vec![0;8],..Section::default()},
        Section{kind:3,data:b"\0alpha\0".to_vec(),..Section::default()},
        Section{kind:2,link:2,info:1,entsize:if f.wide{24}else{16},
            data:[f.symbol(0,0,0),f.symbol(1,1,7)].concat(),..Section::default()}]
}
