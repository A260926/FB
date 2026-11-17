#[path="../task_support/fixture.rs"] mod fixture;
use pdb::{FallibleIterator,TypeIndex,IdData,IdIndex,SymbolData,TypeData};
use std::io::Cursor;
fn main()->pdb::Result<()>{
 let bytes=fixture::image();let original=bytes.clone();let mut file=pdb::PDB::open(Cursor::new(bytes.as_slice()))?;
 assert_eq!(file.pdb_information()?.age,1);let dbi=file.debug_information()?;let module=dbi.modules()?.next()?.expect("module");
 let info=file.module_info(&module)?.expect("module stream");let mut syms=info.symbols()?;let mut refs=Vec::new();
 while let Some(s)=syms.next()?{if let SymbolData::Procedure(p)=s.parse()?{refs.push((p.name.to_string().into_owned(),p.type_index.0));}}
 assert_eq!(refs,vec![("direct".into(),0x1007),("through_id".into(),0x1007),("member_id".into(),0x1008)]);
 let tpi=file.type_information()?;let mut tf=tpi.finder();let mut iter=tpi.iter();let mut count=0;
 while let Some(item)=iter.next()?{tf.update(&iter);count+=1;if item.index()!=TypeIndex(0x1003){let _=item.parse()?;}}assert_eq!(count,11);
 assert!(matches!(tf.find(TypeIndex(0x1007))?.parse()?,TypeData::Procedure(_)));
 assert!(matches!(tf.find(TypeIndex(0x1004))?.parse()?,TypeData::Class(_)));
 let ipi=file.id_information()?;let mut ids=ipi.iter();let mut f=ipi.finder();while ids.next()?.is_some(){f.update(&ids);}
 match f.find(IdIndex(0x1007))?.parse()?{IdData::Function(i)=>assert_eq!(i.function_type.0,0x1009),_=>panic!("function id")}
 match f.find(IdIndex(0x1008))?.parse()?{IdData::MemberFunction(i)=>assert_eq!(i.function_type.0,0x100a),_=>panic!("member id")}
 println!("BASELINE_OK module_procedures=3 tpi_records=11 ipi_records=9 same_raw_index=4103 id_target=4105");
 match tf.find(TypeIndex(0x1003))?.parse(){
  Ok(TypeData::FieldList(f))=>println!("CONTINUATION_OBSERVED {:?}",f.continuation),
  Ok(other)=>println!("CONTINUATION_OBSERVED {:?}",other),
  Err(e)=>println!("CONTINUATION_OBSERVED error={:?}",e)
 }
 assert_eq!(bytes,original);Ok(())
}
