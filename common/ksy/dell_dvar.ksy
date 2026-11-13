meta:
  id: dell_dvar
  title: Dell DVAR Storage
  application: Dell UEFI firmware
  file-extension: dvar
  tags:
    - firmware
  license: CC0-1.0
  ks-version: 0.9
  endian: le

seq:
 - id: signature
   size: 4
 - id: len_store_c
   type: u4
 - id: flags_c
   type: u1
 - id: entries
   type: dvar_entry
   repeat: until
   repeat-until: _.state_c == 0xFF

instances:
 len_store:
  value: 0xFFFFFFFF - len_store_c
 flags:
  value: 0xFF - flags_c
 data_offset:
  value: 9

#TODO: find samples with different flags and types to make them flags insead of immediates

types:
 dvar_entry:
  seq:
  - id: state_c
    type: u1
  - id: flags_c
    type: u1
    if: state_c != 0xFF
  - id: type_c
    type: u1
    if: state_c != 0xFF
  - id: attributes_c
    type: u1
    if: state_c != 0xFF
  - id: namespace_id_c
    type: u1
    if: state_c != 0xFF and (flags == 2 or flags == 6)
  - id: namespace_guid
    size: 16
    if: state_c != 0xFF and flags == 6
  - id: name_id_8_c
    type: u1
    if: state_c != 0xFF and type == 0
  - id: name_id_16_c
    type: u2
    if: state_c != 0xFF and (type == 4 or type == 5)
  - id: len_data_8_c
    type: u1
    if: state_c != 0xFF and (type == 0 or type == 4)
  - id: len_data_16_c
    type: u2
    if: state_c != 0xFF and type == 5
  - id: data_8
    size: len_data_8
    if: state_c != 0xFF and (type == 0 or type == 4)
  - id: data_16
    size: len_data_16
    if: state_c != 0xFF and type == 5
    
  instances:
   state:
    value: 0xFF - state_c
   flags:
    value: 0xFF - flags_c
   type:
    value: 0xFF - type_c
   attributes:
    value: 0xFF - attributes_c
   namespace_id:
    value: 0xFF - namespace_id_c
   name_id_8:
    value: 0xFF - name_id_8_c
   name_id_16:
    value: 0xFFFF - name_id_16_c
   len_data_8:
    value: 0xFF - len_data_8_c
   len_data_16:
    value: 0xFFFF - len_data_16_c
