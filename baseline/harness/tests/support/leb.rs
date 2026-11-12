//! LEB128 encoders used by the line program byte generator.
//!
//! These are deliberately written from the DWARF specification rather than
//! calling into either gimli's writer or its `leb128` module, so that the
//! contract generator stays independent of the production code under test.

/// Encode an unsigned LEB128 value.
pub fn uleb128(mut value: u64, out: &mut Vec<u8>) {
    loop {
        let mut byte = (value & 0x7f) as u8;
        value >>= 7;
        if value != 0 {
            byte |= 0x80;
        }
        out.push(byte);
        if value == 0 {
            break;
        }
    }
}

/// Encode a signed LEB128 value.
pub fn sleb128(mut value: i64, out: &mut Vec<u8>) {
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        let done = value == 0 && byte & 0x40 == 0 || value == -1 && byte & 0x40 != 0;
        if done {
            out.push(byte);
            break;
        }
        out.push(byte | 0x80);
    }
}
