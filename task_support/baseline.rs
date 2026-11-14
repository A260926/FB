use object::{Object,ObjectSection};
mod fixture;
fn main() {
    let bytes=fixture::basic_object();
    if std::env::args().nth(1).as_deref()==Some("--emit") {
        std::fs::write("task_support/basic.macho",&bytes).unwrap(); return;
    }
    let delivered=std::fs::read("task_support/basic.macho").unwrap();
    assert_eq!(bytes,delivered,"delivered image must match its source");
    let file=object::read::macho::MachOFile32::<object::LittleEndian>::parse(bytes.as_slice()).unwrap();
    assert_eq!(file.sections().count(),3);
    let section=file.section_by_name("__debug_info").unwrap();
    let relocs: Vec<_>=section.relocations().collect();
    let plain=relocs.iter().find(|(offset,_)|*offset==4).unwrap();
    assert_eq!(plain.1.size(),32);
    assert!(matches!(plain.1.target(),object::RelocationTarget::Section(object::SectionIndex(1))));
    let mut map=object::RelocationMap::default();
    let (_,normal)=section.relocations().find(|(offset,_)|*offset==4).unwrap();
    map.add(&file,4,normal).unwrap();
    let value=map.relocate(4,7); assert_eq!(value,0x1007);
    println!("NORMAL_MAP={value:#x}");
    if std::env::args().nth(1).as_deref()==Some("--normal") {println!("NORMAL_OK");return;}
    let visible=relocs.iter().filter(|(offset,_)|*offset==0).count();
    assert_eq!(visible,0,"baseline documents original unsupported scattered records");
    println!("SCATTERED_VISIBLE={visible}");println!("BASELINE_OK");
}
