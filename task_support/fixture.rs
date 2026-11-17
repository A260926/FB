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
 procedure_attrs(ret,args,count,0)
}
pub fn procedure_attrs(ret:u32,args:u32,count:u16,attrs:u16)->Vec<u8>{
 let mut r=Vec::new();u32le(&mut r,ret);u16le(&mut r,attrs);u16le(&mut r,count);u32le(&mut r,args);typed(0x1008,r)
}
pub fn arglist(args:&[u32])->Vec<u8>{let mut r=Vec::new();u32le(&mut r,args.len() as u32);for a in args{u32le(&mut r,*a);}typed(0x1201,r)}
pub fn pointer(to:u32)->Vec<u8>{pointer_attrs(to,0x1000c)}
pub fn pointer_attrs(to:u32,attrs:u32)->Vec<u8>{let mut r=Vec::new();u32le(&mut r,to);u32le(&mut r,attrs);typed(0x1002,r)}
pub fn member_function(ret:u32,class:u32,this:u32,attrs:u16,count:u16,args:u32,adjust:u32)->Vec<u8>{
 let mut r=Vec::new();u32le(&mut r,ret);u32le(&mut r,class);u32le(&mut r,this);
 u16le(&mut r,attrs);u16le(&mut r,count);u32le(&mut r,args);u32le(&mut r,adjust);typed(0x1009,r)
}
pub fn structure(name:&str,unique:Option<&str>,forward:bool,fields:u32)->Vec<u8>{
 structure_kind(0x1505,name,unique,forward,fields,if forward{0}else{16})
}
pub fn structure_kind(leaf:u16,name:&str,unique:Option<&str>,forward:bool,fields:u32,size:u16)->Vec<u8>{
 let mut r=Vec::new();u16le(&mut r,if forward{0}else{2});
 u16le(&mut r,if forward{0x80}else{0}|if unique.is_some(){0x200}else{0});
 u32le(&mut r,fields);u32le(&mut r,0);u32le(&mut r,0);u16le(&mut r,size);
 string(&mut r,name);if let Some(n)=unique{string(&mut r,n);}typed(leaf,r)
}
pub fn class(name:&str,unique:Option<&str>,forward:bool,fields:u32)->Vec<u8>{
 structure_kind(0x1504,name,unique,forward,fields,if forward{0}else{16})
}
pub fn bitfield(underlying:u32,length:u8,position:u8)->Vec<u8>{
 let mut r=Vec::new();u32le(&mut r,underlying);r.push(length);r.push(position);typed(0x1205,r)
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
/// An `LF_FIELDLIST` whose entries are appended verbatim. `LF_INDEX` continuations use the
/// modern Microsoft layout: leaf, a required zero `u16`, then the `u32` type index.
pub fn fieldlist(entries:&[Vec<u8>])->Vec<u8>{let mut r=Vec::new();for e in entries{r.extend_from_slice(e);}typed(0x1203,r)}
pub fn lf_index(next:u32)->Vec<u8>{let mut r=Vec::new();u16le(&mut r,0x1404);u16le(&mut r,0);u32le(&mut r,next);r}
pub fn member_raw(name:&str,typ:u32,offset:u16,attrs:u16)->Vec<u8>{
 let mut r=Vec::new();u16le(&mut r,0x150d);u16le(&mut r,attrs);u32le(&mut r,typ);u16le(&mut r,offset);string(&mut r,name);
 let n=(4-r.len()%4)%4;for i in (1..=n).rev(){r.push(0xf0+i as u8);}r
}
pub fn nested_type(name:&str,typ:u32,attrs:u16)->Vec<u8>{
 let mut r=Vec::new();u16le(&mut r,0x1510);u16le(&mut r,attrs);u32le(&mut r,typ);string(&mut r,name);
 let n=(4-r.len()%4)%4;for i in (1..=n).rev(){r.push(0xf0+i as u8);}r
}
pub fn function_id(name:&str,typ:u32)->Vec<u8>{function_id_scoped(0,name,typ)}
pub fn function_id_scoped(scope:u32,name:&str,typ:u32)->Vec<u8>{let mut r=Vec::new();u32le(&mut r,scope);u32le(&mut r,typ);string(&mut r,name);typed(0x1601,r)}
pub fn member_function_id(name:&str,parent:u32,typ:u32)->Vec<u8>{let mut r=Vec::new();u32le(&mut r,parent);u32le(&mut r,typ);string(&mut r,name);typed(0x1602,r)}
pub fn string_id(name:&str)->Vec<u8>{let mut r=Vec::new();u32le(&mut r,0);string(&mut r,name);typed(0x1605,r)}
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
// ---------------------------------------------------------------------------
// Scenario graphs for the procedure type graph regression.
//
// Each scenario is assembled with `TypeBuilder`, which records the index of every record it adds.
// Tests receive the resulting record list plus the named indices, so they never depend on
// hand-counted offsets. All references stay below the referencing record, as CodeView requires.
// ---------------------------------------------------------------------------

/// A small builder that tracks TPI record indices starting at `0x1000`.
pub struct TypeBuilder { records: Vec<Vec<u8>> }
impl TypeBuilder {
 pub fn new()->Self{Self{records:Vec::new()}}
 pub fn index(&self)->u32{0x1000+self.records.len() as u32}
 /// Append a raw record, returning its index.
 pub fn add(&mut self,r:Vec<u8>)->u32{let i=self.index();self.records.push(r);i}
 pub fn modifier(&mut self,underlying:u32,flags:u16)->u32{let mut r=Vec::new();u32le(&mut r,underlying);u16le(&mut r,flags);self.add(typed(0x1001,r))}
 pub fn pointer(&mut self,to:u32,attrs:u32)->u32{self.add(pointer_attrs(to,attrs))}
 pub fn arglist(&mut self,args:&[u32])->u32{self.add(arglist(args))}
 pub fn procedure(&mut self,ret:u32,args:u32,count:u16,attrs:u16)->u32{self.add(procedure_attrs(ret,args,count,attrs))}
 pub fn member_function(&mut self,ret:u32,class:u32,this:u32,attrs:u16,count:u16,args:u32,adjust:u32)->u32{
  self.add(member_function(ret,class,this,attrs,count,args,adjust))
 }
 pub fn fieldlist(&mut self,entries:&[Vec<u8>])->u32{self.add(fieldlist(entries))}
 /// Add a complete class/struct over an existing field list.
 pub fn complete(&mut self,leaf:u16,name:&str,unique:Option<&str>,fields:u32,size:u16)->u32{
  self.add(structure_kind(leaf,name,unique,false,fields,size))
 }
 pub fn forward(&mut self,leaf:u16,name:&str,unique:Option<&str>)->u32{
  self.add(structure_kind(leaf,name,unique,true,0,0))
 }
 pub fn finish(self)->Vec<Vec<u8>>{self.records}
}

/// Named indices for the `origin` scenario.
pub struct OriginIndices {
 pub modifier:u32,pub int_ptr:u32,pub args:u32,pub proc:u32,pub this_ptr:u32,
 pub method_args:u32,pub method:u32,pub fields:u32,pub class_c:u32,pub tpi_same:u32,
 pub class_c_forward:u32,pub func_id:u32,pub method_id:u32,
}
/// The `origin` scenario: an ordinary procedure, an `_ID` procedure, a member function, and a TPI
/// record that deliberately shares its number with the IPI `LF_FUNC_ID`.
///
/// The member function's class type is the complete `C`, while `LF_MFUNC_ID`'s parent is a forward
/// declaration of `C`, so the parent must be canonicalized before it can be compared.
pub fn origin()->(Vec<Vec<u8>>,Vec<Vec<u8>>,OriginIndices){
 let mut b=TypeBuilder::new();
 let modifier=b.modifier(0x74,1); // const int32
 let int_ptr=b.pointer(0x74,0x1000c|0x400|0x200); // const volatile int*
 let args=b.arglist(&[int_ptr,0x74]);
 let proc=b.procedure(modifier,args,2,0x0040);
 let fields=b.fieldlist(&[member_raw("a",0x74,0,3)]);
 let class_c=b.complete(0x1504,"C",Some(".?AVC@@"),fields,4);
 let class_c_forward=b.forward(0x1504,"C",Some(".?AVC@@"));
 let this_ptr=b.pointer(class_c,0x1000c);
 let method_args=b.arglist(&[int_ptr,0x74]);
 let method=b.member_function(0x74,class_c,this_ptr,0x0040,2,method_args,4);
 let tpi_same=b.procedure(0x74,args,2,0x0040); // same number as the IPI id below
 let func_id=0x1009;
 let method_id=0x100a;
 let idx=OriginIndices{modifier,int_ptr,args,proc,this_ptr,method_args,method,fields,class_c,tpi_same,
  class_c_forward,func_id,method_id};
 let mut ids=Vec::new();
 for _ in 0..(func_id-0x1000){ids.push(string_id("scope"));}
 ids.push(function_id("origin_func",proc));
 ids.push(member_function_id("origin_method",class_c_forward,method));
 (b.finish(),ids,idx)
}

/// Named indices for the `forward` scenario.
pub struct ForwardIndices {
 pub display_forward:u32,pub display_def_a:u32,pub display_def_b:u32,
 pub unique_forward:u32,pub unique_decoy:u32,pub unique_real:u32,
 pub unbound:u32,pub proc:u32,
 pub explicit_a:u32,pub explicit_b:u32,pub twin_proc:u32,
 pub shape_forward:u32,pub shape_class_def:u32,
 pub sole_def:u32,pub sole_forward:u32,pub named_def:u32,pub named_forward:u32,
}
/// The `forward` scenario: unique-name matching, display-name matching, a zero-candidate forward,
/// an ambiguous forward, and explicitly referenced definitions that share display names.
pub fn forward()->(Vec<Vec<u8>>,ForwardIndices){
 let mut b=TypeBuilder::new();
 let display_forward=b.forward(0x1505,"Dup",None);
 let f1=b.fieldlist(&[member_raw("x",0x74,0,3)]);
 let display_def_a=b.complete(0x1505,"Dup",None,f1,8);
 let f2=b.fieldlist(&[member_raw("x",0x74,0,3)]);
 let display_def_b=b.complete(0x1505,"Dup",None,f2,8);
 let unique_forward=b.forward(0x1505,"AliasA",Some(".?AUReal@@"));
 let f3=b.fieldlist(&[member_raw("y",0x74,0,3)]);
 let unique_decoy=b.complete(0x1505,"AliasA",Some(".?AUDecoy@@"),f3,8);
 let f4=b.fieldlist(&[member_raw("z",0x74,0,3)]);
 let unique_real=b.complete(0x1505,"DifferentName",Some(".?AUReal@@"),f4,8);
 let unbound=b.forward(0x1505,"Missing",Some(".?AUMissing@@"));
 let int_ptr=b.pointer(0x74,0x1000c);
 let args=b.arglist(&[int_ptr]);
 let proc=b.procedure(unbound,args,1,0);
 let f5=b.fieldlist(&[member_raw("t1",0x74,0,3)]);
 let explicit_a=b.complete(0x1505,"Twin",Some(".?AUTwin1@@"),f5,4);
 let f6=b.fieldlist(&[member_raw("t2",0x74,0,3)]);
 let explicit_b=b.complete(0x1505,"Twin",Some(".?AUTwin2@@"),f6,4);
 let twin_args=b.arglist(&[]);
 let twin_proc=b.procedure(explicit_a,twin_args,0,0);
 let shape_forward=b.forward(0x1505,"Shape",None);
 let f7=b.fieldlist(&[member_raw("w",0x74,0,3)]);
 let shape_class_def=b.complete(0x1504,"Shape",None,f7,4);
 // A sole display-name match whose complete definition is recorded *before* the forward.
 let f8=b.fieldlist(&[member_raw("s",0x74,0,3)]);
 let sole_def=b.complete(0x1505,"Sole",None,f8,4);
 let sole_forward=b.forward(0x1505,"Sole",None);
 // A display-name-only forward whose sole matching complete definition *does* carry a unique
 // name: the policy must still consider it.
 let f9=b.fieldlist(&[member_raw("u",0x74,0,3)]);
 let named_def=b.complete(0x1505,"Named",Some(".?AUNamed@@"),f9,4);
 let named_forward=b.forward(0x1505,"Named",None);
 let idx=ForwardIndices{display_forward,display_def_a,display_def_b,unique_forward,unique_decoy,
  unique_real,unbound,proc,explicit_a,explicit_b,twin_proc,shape_forward,shape_class_def,
  sole_def,sole_forward,named_def,named_forward};
 (b.finish(),idx)
}

/// Named indices for the `recursive` scenario.
pub struct RecursiveIndices {
 pub a_ptr:u32,pub b_ptr:u32,pub a_def:u32,pub b_def:u32,pub a_forward:u32,pub b_forward:u32,
 pub proc_a:u32,pub proc_b:u32,pub shared:u32,pub shared_proc:u32,
}
/// The `recursive` scenario: mutually recursive classes through pointers (each reached through a
/// forward reference), a field list split across `LF_INDEX`, and a class with two members of the
/// same type.
pub fn recursive()->(Vec<Vec<u8>>,RecursiveIndices){
 let mut b=TypeBuilder::new();
 // Forward declarations come first so the pointers below can reference them.
 let a_forward=b.forward(0x1505,"A",None);
 let b_forward=b.forward(0x1505,"B",None);
 let a_ptr=b.pointer(b_forward,0x1000c); // A's field points at B
 let b_ptr=b.pointer(a_forward,0x1000c); // B's field points at A
 let a_fields=b.fieldlist(&[member_raw("b",a_ptr,0,3)]);
 let a_def=b.complete(0x1505,"A",None,a_fields,16);
 let b_fields=b.fieldlist(&[member_raw("a",b_ptr,8,3)]);
 let b_def=b.complete(0x1505,"B",None,b_fields,16);
 let args=b.arglist(&[]);
 let proc_a=b.procedure(a_forward,args,0,0);
 let proc_b=b.procedure(b_forward,args,0,0);
 // Shared: head list holds `first` and continues into a second list holding `second`.
 // The head's LF_INDEX points at a record only added below, so reserve the head first and patch
 // it once the tail index is known.
 let head_slot=b.index();
 let tail_index=head_slot+1; // the tail is added immediately after the head
 let head=b.fieldlist(&[member_raw("first",0x74,0,3),lf_index(tail_index)]);
 assert_eq!(head,head_slot);
 let tail=b.fieldlist(&[member_raw("second",0x74,4,3)]);
 assert_eq!(tail,tail_index);
 let shared=b.complete(0x1505,"Shared",None,head,8);
 let shared_proc=b.procedure(shared,args,0,0);
 let idx=RecursiveIndices{a_ptr,b_ptr,a_def,b_def,a_forward,b_forward,proc_a,proc_b,shared,shared_proc};
 (b.finish(),idx)
}

/// Build a synthetic image from a scenario's TPI and IPI records plus module procedures.
pub fn image_scenario(types:&[Vec<u8>],ids:&[Vec<u8>],procs:&[(u16,u32,&str)])->Vec<u8>{
 image_with(types,ids,&module_stream_with(procs))
}
pub fn streams()->Vec<Option<Vec<u8>>>{
 streams_with(&types(),&ids(),&module_stream())
}
// Each process symbol has a matching end; offsets include the C13 signature.
pub fn module_stream()->Vec<u8>{
 module_stream_with(&[(0x1110,0x1007,"direct"),(0x1147,0x1007,"through_id"),(0x1146,0x1008,"member_id")])
}
pub fn streams_with(types:&[Vec<u8>],ids:&[Vec<u8>],symbols:&[u8])->Vec<Option<Vec<u8>>>{
 let mut module=vec![0u8;64];p16(&mut module,4,1);p32(&mut module,12,3);
 p16(&mut module,34,5);p32(&mut module,36,symbols.len() as u32);
 string(&mut module,"synthetic.obj");string(&mut module,"synthetic.obj");while module.len()%4!=0{module.push(0);}
 let mut dbi=vec![0u8;64];p32(&mut dbi,0,0xffffffff);p32(&mut dbi,4,19990903);p32(&mut dbi,8,1);
 for o in [12,16,20]{p16(&mut dbi,o,0xffff);}p32(&mut dbi,24,module.len() as u32);p16(&mut dbi,58,0x8664);dbi.extend(module);
 let mut info=vec![0u8;48];p32(&mut info,0,20000404);p32(&mut info,8,1);info[12..28].copy_from_slice(&[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]);p32(&mut info,36,1);
 vec![None,Some(info),Some(type_stream(types)),Some(dbi),Some(type_stream(ids)),Some(symbols.to_vec())]
}
/// Build a module symbol stream containing the given `(kind, type_index, name)` procedures.
pub fn module_stream_with(procs:&[(u16,u32,&str)])->Vec<u8>{
 let mut out=Vec::new();u32le(&mut out,4);
 for &(kind,index,name) in procs {
  let start=out.len();let mut b=Vec::new();u16le(&mut b,kind);
  for x in [0,0,0,1,0,1,index,0]{u32le(&mut b,x);}u16le(&mut b,1);b.push(0);string(&mut b,name);
  while (b.len()+2)%4!=0{b.push(0);}
  u16le(&mut out,b.len() as u16);out.extend(b);
  let end=out.len() as u32;p32(&mut out,start+8,end);
  u16le(&mut out,2);u16le(&mut out,if kind==0x1110{6}else{0x114f});
 }out
}
pub fn image_with(types:&[Vec<u8>],ids:&[Vec<u8>],symbols:&[u8])->Vec<u8>{msf(&streams_with(types,ids,symbols))}
/// Build an image whose IPI stream is absent.
pub fn image_without_ipi(types:&[Vec<u8>],symbols:&[u8])->Vec<u8>{
 let mut streams=streams_with(types,&[],symbols);
 streams[4]=None;
 msf(&streams)
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
