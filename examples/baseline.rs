use gimli::{BigEndian, DebugPubNames, LittleEndian, Reader};
fn bytes(be: bool) -> Vec<u8> {
    let mut out = Vec::new();
    let u32s = |v: u32| if be { v.to_be_bytes() } else { v.to_le_bytes() };
    out.extend(u32s(24));
    out.extend(if be { 2u16.to_be_bytes() } else { 2u16.to_le_bytes() });
    out.extend(u32s(0x100)); out.extend(u32s(0x40));
    out.extend(u32s(0x20)); out.extend(b"local\0"); out.extend(u32s(0));
    out
}
fn main() {
    let le = bytes(false); let be = bytes(true);
    let mut a = DebugPubNames::new(&le, LittleEndian).items();
    let e = a.next().unwrap().unwrap();
    assert_eq!(e.name().to_slice().unwrap().as_ref(), b"local");
    assert_eq!(e.unit_header_offset().0, 0x100);
    assert_eq!(e.die_offset().0, 0x20); assert!(a.next().unwrap().is_none());
    let mut b = DebugPubNames::new(&be, BigEndian).items();
    let e = b.next().unwrap().unwrap();
    assert_eq!(e.name().to_slice().unwrap().as_ref(), b"local");
    assert_eq!(e.unit_header_offset().0, 0x100);
    assert_eq!(e.die_offset().0, 0x20); assert!(b.next().unwrap().is_none());
    println!("GIMLI_PUBNAMES_LE_BE_OK");
}
