#[path="../task_support/fixture.rs"] mod fixture;
use pdb::FallibleIterator;
fn main()->Result<(),Box<dyn std::error::Error>>{
 let bytes=fixture::image();let expected=fixture::symbol_stream().1;
 let mut file=pdb::PDB::open(std::io::Cursor::new(bytes))?;
 let info=file.pdb_information()?;assert_eq!(info.age,1);assert_eq!(info.stream_names()?.iter().count(),0);
 assert_eq!(file.type_information()?.len(),0);
 let symbols=file.global_symbols()?;let mut iter=symbols.iter();let mut got=Vec::new();
 while let Some(s)=iter.next()? {let data=s.parse()?;println!("symbol offset={} name={}",s.index().0,data.name().unwrap().to_string());got.push(s.index().0);}
 assert_eq!(got,expected);
 let global=file.raw_stream(pdb::StreamIndex(6))?.unwrap();let public=file.raw_stream(pdb::StreamIndex(7))?.unwrap();
 assert_eq!(&global.as_slice()[..4],&[255,255,255,255]);
 println!("PDB_BASELINE_OK symbols={} gsi_bytes={} psi_bytes={} record_offsets_preserved=true",got.len(),global.as_slice().len(),public.as_slice().len());
 Ok(())
}
