use gimli::{DebugLine, DebugLineOffset, LittleEndian};
fn main() {
    let mut header = vec![2, 1, 1, 251, 14, 13];
    header.extend_from_slice(&[0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1]);
    header.extend_from_slice(b"\0main.c\0\0\0\0\0");
    let mut body = Vec::new();
    body.extend_from_slice(&4u16.to_le_bytes());
    body.extend_from_slice(&(header.len() as u32).to_le_bytes());
    body.extend(header);
    for addr in [0x1000u64, 0x2000] {
        body.extend_from_slice(&[0, 9, 2]);
        body.extend_from_slice(&addr.to_le_bytes());
        body.extend_from_slice(&[1, 2, 3, 3, 4, 1, 0, 1, 1]);
    }
    let mut bytes = (body.len() as u32).to_le_bytes().to_vec();
    bytes.extend(body);
    let program = DebugLine::new(&bytes, LittleEndian).program(DebugLineOffset(0), 8, None, None).unwrap();
    let mut rows = program.clone().rows();
    let mut observed = Vec::new();
    while let Some((_, row)) = rows.next_row().unwrap() {
        observed.push((row.address(), row.op_index(), row.line().unwrap().get(), row.end_sequence()));
    }
    assert_eq!(observed, vec![(0x1000,0,1,false),(0x1006,0,5,false),(0x1006,0,5,true),
                            (0x2000,0,1,false),(0x2006,0,5,false),(0x2006,0,5,true)]);
    let (complete, sequences) = program.sequences().unwrap();
    assert_eq!(sequences.len(), 2);
    for (i, seq) in sequences.iter().enumerate() {
        let mut resumed = complete.resume_from(seq);
        let mut count = 0;
        while let Some((_, row)) = resumed.next_row().unwrap() {
            assert_eq!(row.address(), observed[i*3+count].0); count+=1;
        }
        assert_eq!(count,3);
    }
    println!("GIMLI_LINE_BASELINE_OK rows=6 sequences=2 resumed=6");
}
