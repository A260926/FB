// Legal metadata-only synthetic PDB. No PE image, process, or external input.
#![allow(dead_code)]
pub fn u16le(v:&mut Vec<u8>,x:u16){v.extend_from_slice(&x.to_le_bytes())}
pub fn u32le(v:&mut Vec<u8>,x:u32){v.extend_from_slice(&x.to_le_bytes())}
pub fn p16(v:&mut [u8],o:usize,x:u16){v[o..o+2].copy_from_slice(&x.to_le_bytes())}
pub fn p32(v:&mut [u8],o:usize,x:u32){v[o..o+4].copy_from_slice(&x.to_le_bytes())}
pub fn string(v:&mut Vec<u8>,s:&str){v.extend_from_slice(s.as_bytes());v.push(0)}
// CodeView type records, including LF_PAD bytes, are four-byte aligned.
pub fn record(mut body:Vec<u8>)->Vec<u8>{
 let n=(4-(body.len()+2)%4)%4;for i in (1..=n).rev(){body.push(0xf0+i as u8);}
 let mut r=Vec::new();u16le(&mut r,body.len() as u16);r.extend(body);r
}
pub fn typed(leaf:u16,bytes:Vec<u8>)->Vec<u8>{let mut r=Vec::new();u16le(&mut r,leaf);r.extend(bytes);record(r)}
pub fn type_stream(records:&[Vec<u8>])->Vec<u8>{
 let mut out=vec![0u8;56];p32(&mut out,0,20040203);p32(&mut out,4,56);
 p32(&mut out,8,0x1000);p32(&mut out,12,0x1000+records.len() as u32);
 p32(&mut out,16,records.iter().map(|r|r.len() as u32).sum());
 p16(&mut out,20,0xffff);p16(&mut out,22,0xffff);p32(&mut out,24,4);p32(&mut out,28,4096);
 for r in records{out.extend(r);}out
}
pub fn procedure(ret:u32,args:u32,count:u16)->Vec<u8>{
 let mut r=Vec::new();u32le(&mut r,ret);u16le(&mut r,0);u16le(&mut r,count);u32le(&mut r,args);typed(0x1008,r)
}
pub fn arglist(args:&[u32])->Vec<u8>{let mut r=Vec::new();u32le(&mut r,args.len() as u32);for a in args{u32le(&mut r,*a);}typed(0x1201,r)}
pub fn pointer(to:u32)->Vec<u8>{let mut r=Vec::new();u32le(&mut r,to);u32le(&mut r,0x1000c);typed(0x1002,r)}
pub fn structure(name:&str,unique:Option<&str>,forward:bool,fields:u32)->Vec<u8>{
 let mut r=Vec::new();u16le(&mut r,if forward{0}else{2});
 u16le(&mut r,if forward{0x80}else{0}|if unique.is_some(){0x200}else{0});
 u32le(&mut r,fields);u32le(&mut r,0);u32le(&mut r,0);u16le(&mut r,if forward{0}else{16});
 string(&mut r,name);if let Some(n)=unique{string(&mut r,n);}typed(0x1505,r)
}
pub fn member(name:&str,typ:u32,offset:u16)->Vec<u8>{
 let mut r=Vec::new();u16le(&mut r,0x150d);u16le(&mut r,3);u32le(&mut r,typ);u16le(&mut r,offset);string(&mut r,name);
 let n=(4-r.len()%4)%4;for i in (1..=n).rev(){r.push(0xf0+i as u8);}r
}
pub fn fields(members:&[Vec<u8>],continuation:Option<u32>)->Vec<u8>{
 let mut r=Vec::new();for m in members{r.extend(m);}
 if let Some(next)=continuation{u16le(&mut r,0x1404);u16le(&mut r,0);u32le(&mut r,next);}
 typed(0x1203,r)
}
pub fn function_id(name:&str,typ:u32)->Vec<u8>{let mut r=Vec::new();u32le(&mut r,0);u32le(&mut r,typ);string(&mut r,name);typed(0x1601,r)}
pub fn member_function_id(name:&str,parent:u32,typ:u32)->Vec<u8>{let mut r=Vec::new();u32le(&mut r,parent);u32le(&mut r,typ);string(&mut r,name);typed(0x1602,r)}
pub fn types()->Vec<Vec<u8>>{
 let mut modifier=Vec::new();u32le(&mut modifier,0x1001);u16le(&mut modifier,1);
 let mut method=Vec::new();for i in [0x74,0x1004,0x1001]{u32le(&mut method,i);}u16le(&mut method,0);u16le(&mut method,1);u32le(&mut method,0x1006);u32le(&mut method,0);
 // Only lower-index record references, including LF_INDEX, as emitted by CodeView producers.
 vec![structure("Node",Some(".?AUNode@@"),true,0), // 1000
 pointer(0x1000), // 1001
 fields(&[member("next",0x1001,8)],None), // 1002: continuation tail emitted first
 fields(&[member("value",0x74,0)],Some(0x1002)), // 1003
 structure("Node",Some(".?AUNode@@"),false,0x1003), // 1004
 typed(0x1001,modifier), // 1005
 arglist(&[0x1001]),procedure(0x74,0x1006,1), // 1006,1007
 arglist(&[0x1005]),procedure(0x03,0x1008,1), // 1008,1009
 typed(0x1009,method)] // 100a LF_MFUNCTION
}
pub fn ids()->Vec<Vec<u8>>{
 let mut out=Vec::new();for _ in 0..7{let mut b=Vec::new();u32le(&mut b,0);string(&mut b,"scope");out.push(typed(0x1605,b));}
 out.push(function_id("through_id",0x1009));out.push(member_function_id("member_id",0x1004,0x100a));out
}
// Each process symbol has a matching end; offsets include the C13 signature.
pub fn module_stream()->Vec<u8>{
 let mut out=Vec::new();u32le(&mut out,4);
 for (kind,index,name) in [(0x1110,0x1007,"direct"),(0x1147,0x1007,"through_id"),(0x1146,0x1008,"member_id")] {
  let start=out.len();let mut b=Vec::new();u16le(&mut b,kind);
  for x in [0,0,0,1,0,1,index,0]{u32le(&mut b,x);}u16le(&mut b,1);b.push(0);string(&mut b,name);
  while (b.len()+2)%4!=0{b.push(0);}
  u16le(&mut out,b.len() as u16);out.extend(b);
  let end=out.len() as u32;p32(&mut out,start+8,end);
  u16le(&mut out,2);u16le(&mut out,if kind==0x1110{6}else{0x114f});
 }out
}
pub fn streams()->Vec<Option<Vec<u8>>>{
 let symbols=module_stream();let mut module=vec![0u8;64];p16(&mut module,4,1);p32(&mut module,12,3);
 p16(&mut module,34,5);p32(&mut module,36,symbols.len() as u32);
 string(&mut module,"synthetic.obj");string(&mut module,"synthetic.obj");while module.len()%4!=0{module.push(0);}
 let mut dbi=vec![0u8;64];p32(&mut dbi,0,0xffffffff);p32(&mut dbi,4,19990903);p32(&mut dbi,8,1);
 for o in [12,16,20]{p16(&mut dbi,o,0xffff);}p32(&mut dbi,24,module.len() as u32);p16(&mut dbi,58,0x8664);dbi.extend(module);
 let mut info=vec![0u8;48];p32(&mut info,0,20000404);p32(&mut info,8,1);info[12..28].copy_from_slice(&[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]);p32(&mut info,36,1);
 vec![None,Some(info),Some(type_stream(&types())),Some(dbi),Some(type_stream(&ids())),Some(symbols)]
}
// Small MSF directory writer inherited from the previous fixed-source scaffold.
pub fn msf(streams:&[Option<Vec<u8>>])->Vec<u8>{
 let mut directory=Vec::new();u32le(&mut directory,streams.len() as u32);
 for s in streams{u32le(&mut directory,s.as_ref().map_or(u32::MAX,|x|x.len() as u32));}
 let mut next=5u32;let mut pages=Vec::new();
 for s in streams{let mut own=Vec::new();if let Some(s)=s{for _ in s.chunks(512){own.push(next);u32le(&mut directory,next);next+=1;}}pages.push(own);}
 assert!(directory.len()<=512);let mut file=vec![0u8;((next as usize)*512).max(4096)];
 file[..32].copy_from_slice(b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0");
 p32(&mut file,32,512);p32(&mut file,36,1);let count=(file.len()/512) as u32;p32(&mut file,40,count);
 p32(&mut file,44,directory.len() as u32);p32(&mut file,52,4);p32(&mut file,4*512,3);
 file[3*512..3*512+directory.len()].copy_from_slice(&directory);
 for (s,own) in streams.iter().zip(pages){if let Some(s)=s{for (chunk,page) in s.chunks(512).zip(own){let o=page as usize*512;file[o..o+chunk.len()].copy_from_slice(chunk);}}}file
}
pub fn image()->Vec<u8>{msf(&streams())}
