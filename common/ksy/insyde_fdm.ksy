meta:
  id: insyde_fdm
  title: Insyde Flash Device Map
  application: Insyde-based UEFI firmware
  file-extension: fdm
  tags:
    - firmware
  license: CC0-1.0
  ks-version: 0.9
  endian: le

seq:
- id: signature
  type: u4
- id: store_size
  type: u4
- id: data_offset
  type: u4
- id: entry_size
  type: u4
- id: entry_format
  type: u1
- id: revision
  type: u1
- id: num_extensions
  type: u1
- id: checksum
  type: u1
- id: fd_base_address
  type: u8
instances:
 valid_entry_size:
  value: 0x54
 valid_entry_format:
  value: 0
 entries:
  pos: data_offset
  type: fdm_entries
  size: store_size - data_offset
  if: entry_size == valid_entry_size and entry_format == valid_entry_format

types:
 fdm_entries:
  seq:
   - id: entries
     type: fdm_entry
     repeat: eos
     
 fdm_entry:
  seq:
  - id: guid
    size: 16
  - id: region_id
    size: 16
  - id: region_offset
    type: u8
  - id: region_size
    type: u8
  - id: attributes
    type: u4
  - id: hash
    size: 0x20
  instances:
   region_base:
     value: _root.fd_base_address.as<u4> + region_offset.as<u4>
