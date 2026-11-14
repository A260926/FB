#[path="fixture.rs"] mod fixture;
use fixture::{Format,Section};
fn main() {
    let normal_only=std::env::args().any(|a|a=="--normal");
    for wide in [false,true] { for little in [false,true] {
        let f=Format{wide,little}; let sections=fixture::ordinary(f); let bytes=f.image(&sections);
        let elf=goblin::elf::Elf::parse(&bytes).unwrap(); let symbol=elf.syms.get(1).unwrap();
        assert_eq!(symbol.st_shndx,1);assert_eq!(elf.strtab.get_at(symbol.st_name),Some("alpha"));
        assert_eq!(elf.syms.iter().nth(1).unwrap(),symbol);assert_eq!(elf.syms.to_vec()[1],symbol);
        println!("NORMAL_OK class={} endian={}",if wide{64}else{32},if little{"LE"}else{"BE"});
        if !normal_only {
            let mut extended=sections; extended[3].data=[f.symbol(0,0,0),f.symbol(1,0xffff,7)].concat();
            extended.push(Section{kind:18,link:3,entsize:4,data:f.words(&[0,65536]),..Section::default()});
            extended.push(Section{kind:3,data:b"\0beta\0".to_vec(),..Section::default()});
            extended.push(Section{kind:11,link:5,info:1,entsize:if wide{24}else{16},data:[f.symbol(0,0,0),f.symbol(1,0xffff,9)].concat(),..Section::default()});
            extended.push(Section{kind:18,link:6,entsize:4,data:f.words(&[0,65521]),..Section::default()});
            extended.resize(65537,Section::default());extended[65536].kind=1;extended[65521].kind=1;
            let b=f.image(&extended);let e=goblin::elf::Elf::parse(&b).unwrap();
            assert_eq!(e.dynsyms.len(),0);
            println!("MISSING_DYNSYM class={} endian={} declared_table=6 parsed_symbols=0 expected=2",if wide{64}else{32},if little{"LE"}else{"BE"});
            let got=e.syms.get(1).unwrap().st_shndx;assert_eq!(got,65535);
            assert_eq!(e.syms.iter().nth(1).unwrap().st_shndx,got);
            println!("MISSING_SHNDX class={} endian={} raw_returned={} defined_section=65536 sections={}",if wide{64}else{32},if little{"LE"}else{"BE"},got,e.section_headers.len());
        }
    }}
    println!("BASELINE_OK");
}
