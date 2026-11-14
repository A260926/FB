// Metadata-only synthetic MSF7/PDB. No executable image or external fixture.
fn u16le(v:&mut Vec<u8>,x:u16){v.extend_from_slice(&x.to_le_bytes())}
fn u32le(v:&mut Vec<u8>,x:u32){v.extend_from_slice(&x.to_le_bytes())}
fn p16(v:&mut [u8],o:usize,x:u16){v[o..o+2].copy_from_slice(&x.to_le_bytes())}
fn p32(v:&mut [u8],o:usize,x:u32){v[o..o+4].copy_from_slice(&x.to_le_bytes())}
pub fn symbol_stream()->(Vec<u8>,Vec<u32>){
 let mut out=Vec::new();let mut offsets=Vec::new();
 for (name,public,section,offset) in [("Alpha",false,1,0x10),("alpha",true,1,0x20),("Alias",true,1,0x20),("Alias",true,2,0x20),("Beta",false,1,0x30)] {
  offsets.push(out.len() as u32);let mut rec=Vec::new();
  u16le(&mut rec,if public{0x110e}else{0x110d});
  u32le(&mut rec,if public{3}else{0x74});u32le(&mut rec,offset);u16le(&mut rec,section);
  rec.extend_from_slice(name.as_bytes());rec.push(0);
  while (rec.len()+2)%4!=0 {rec.push(0);}
  u16le(&mut out,rec.len() as u16);out.extend(rec);
 }
 (out,offsets)
}
// The bucket numbers here are fixed input material, not a hash implementation.
pub fn gsi(mut records:Vec<(u32,u32)>)->Vec<u8>{
 records.sort_by_key(|x|(x.0,x.1));let mut bitmap=vec![0u8;516];let mut starts=Vec::new();let mut last=None;
 for (i,(bucket,_)) in records.iter().enumerate(){if last!=Some(*bucket){bitmap[*bucket as usize/8]|=1<<(*bucket%8);u32le(&mut starts,i as u32*12);last=Some(*bucket);}}
 let mut out=Vec::new();u32le(&mut out,0xffffffff);u32le(&mut out,0xf12f091a);
 u32le(&mut out,records.len() as u32*8);u32le(&mut out,(bitmap.len()+starts.len()) as u32);
 for (_,offset) in records{u32le(&mut out,offset+1);u32le(&mut out,1);}
 out.extend(bitmap);out.extend(starts);out
}
pub fn streams()->Vec<Option<Vec<u8>>>{
 let (symbols,off)=symbol_stream();
 let globals=gsi(vec![(2640,off[0]),(2710,off[4])]);
 let publics=gsi(vec![(2640,off[1]),(122,off[2]),(122,off[3])]);
 let mut psi=vec![0u8;28];p32(&mut psi,0,publics.len() as u32);p32(&mut psi,4,12);psi.extend(publics);
 // Same-address aliases ordered by exact spelling; offsets are zero-based here.
 for i in [2,1,3]{u32le(&mut psi,off[i]);}
 let mut dbi=vec![0u8;64];p32(&mut dbi,0,0xffffffff);p32(&mut dbi,4,19990903);p32(&mut dbi,8,1);
 p16(&mut dbi,12,6);p16(&mut dbi,16,7);p16(&mut dbi,20,5);p16(&mut dbi,58,0x8664);
 let mut info=vec![0u8;48];p32(&mut info,0,20000404);p32(&mut info,8,1);
 info[12..28].copy_from_slice(&[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]);p32(&mut info,36,1);
 let mut tpi=vec![0u8;56];p32(&mut tpi,0,20040203);p32(&mut tpi,4,56);p32(&mut tpi,8,0x1000);p32(&mut tpi,12,0x1000);
 p16(&mut tpi,20,0xffff);p16(&mut tpi,22,0xffff);p32(&mut tpi,24,4);p32(&mut tpi,28,4096);
 vec![None,Some(info),Some(tpi),Some(dbi),None,Some(symbols),Some(globals),Some(psi)]
}
pub fn msf(streams:&[Option<Vec<u8>>])->Vec<u8>{
 let mut directory=Vec::new();u32le(&mut directory,streams.len() as u32);
 for s in streams{u32le(&mut directory,s.as_ref().map_or(u32::MAX,|x|x.len() as u32));}
 let mut next=5u32;let mut pages=Vec::new();
 for s in streams {let mut own=Vec::new();if let Some(s)=s {for _ in s.chunks(512){own.push(next);u32le(&mut directory,next);next+=1;}}pages.push(own);}
 assert!(directory.len()<=512);let mut file=vec![0u8;((next as usize)*512).max(4096)];
 file[..32].copy_from_slice(b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0");
 p32(&mut file,32,512);p32(&mut file,36,1);let count=(file.len()/512) as u32;p32(&mut file,40,count);
 p32(&mut file,44,directory.len() as u32);p32(&mut file,52,4);p32(&mut file,4*512,3);
 file[3*512..3*512+directory.len()].copy_from_slice(&directory);
 for (s,own) in streams.iter().zip(pages){if let Some(s)=s{for (chunk,page) in s.chunks(512).zip(own){let o=page as usize*512;file[o..o+chunk.len()].copy_from_slice(chunk);}}}
 file
}
pub fn image()->Vec<u8>{msf(&streams())}
