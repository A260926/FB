// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

#include "dell_dvar.h"

dell_dvar_t::dell_dvar_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent, dell_dvar_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root ? p__root : this;
    m_entries = nullptr;
    f_data_offset = false;
    f_flags = false;
    f_len_store = false;
    _read();
}

void dell_dvar_t::_read() {
    m_signature = m__io->read_bytes(4);
    m_len_store_c = m__io->read_u4le();
    m_flags_c = m__io->read_u1();
    m_entries = std::unique_ptr<std::vector<std::unique_ptr<dvar_entry_t>>>(new std::vector<std::unique_ptr<dvar_entry_t>>());
    {
        int i = 0;
        dvar_entry_t* _;
        do {
            _ = new dvar_entry_t(m__io, this, m__root);
            m_entries->push_back(std::move(std::unique_ptr<dvar_entry_t>(_)));
            i++;
        } while (!(_->state_c() == 255));
    }
}

dell_dvar_t::~dell_dvar_t() {
    _clean_up();
}

void dell_dvar_t::_clean_up() {
}

dell_dvar_t::dvar_entry_t::dvar_entry_t(kaitai::kstream* p__io, dell_dvar_t* p__parent, dell_dvar_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    f_attributes = false;
    f_flags = false;
    f_len_data_16 = false;
    f_len_data_8 = false;
    f_name_id_16 = false;
    f_name_id_8 = false;
    f_namespace_id = false;
    f_state = false;
    f_type = false;
    _read();
}

void dell_dvar_t::dvar_entry_t::_read() {
    m_state_c = m__io->read_u1();
    n_flags_c = true;
    if (state_c() != 255) {
        n_flags_c = false;
        m_flags_c = m__io->read_u1();
    }
    n_type_c = true;
    if (state_c() != 255) {
        n_type_c = false;
        m_type_c = m__io->read_u1();
    }
    n_attributes_c = true;
    if (state_c() != 255) {
        n_attributes_c = false;
        m_attributes_c = m__io->read_u1();
    }
    n_namespace_id_c = true;
    if ( ((state_c() != 255) && ( ((flags() == 2) || (flags() == 6)) )) ) {
        n_namespace_id_c = false;
        m_namespace_id_c = m__io->read_u1();
    }
    n_namespace_guid = true;
    if ( ((state_c() != 255) && (flags() == 6)) ) {
        n_namespace_guid = false;
        m_namespace_guid = m__io->read_bytes(16);
    }
    n_name_id_8_c = true;
    if ( ((state_c() != 255) && (type() == 0)) ) {
        n_name_id_8_c = false;
        m_name_id_8_c = m__io->read_u1();
    }
    n_name_id_16_c = true;
    if ( ((state_c() != 255) && ( ((type() == 4) || (type() == 5)) )) ) {
        n_name_id_16_c = false;
        m_name_id_16_c = m__io->read_u2le();
    }
    n_len_data_8_c = true;
    if ( ((state_c() != 255) && ( ((type() == 0) || (type() == 4)) )) ) {
        n_len_data_8_c = false;
        m_len_data_8_c = m__io->read_u1();
    }
    n_len_data_16_c = true;
    if ( ((state_c() != 255) && (type() == 5)) ) {
        n_len_data_16_c = false;
        m_len_data_16_c = m__io->read_u2le();
    }
    n_data_8 = true;
    if ( ((state_c() != 255) && ( ((type() == 0) || (type() == 4)) )) ) {
        n_data_8 = false;
        m_data_8 = m__io->read_bytes(len_data_8());
    }
    n_data_16 = true;
    if ( ((state_c() != 255) && (type() == 5)) ) {
        n_data_16 = false;
        m_data_16 = m__io->read_bytes(len_data_16());
    }
}

dell_dvar_t::dvar_entry_t::~dvar_entry_t() {
    _clean_up();
}

void dell_dvar_t::dvar_entry_t::_clean_up() {
    if (!n_flags_c) {
    }
    if (!n_type_c) {
    }
    if (!n_attributes_c) {
    }
    if (!n_namespace_id_c) {
    }
    if (!n_namespace_guid) {
    }
    if (!n_name_id_8_c) {
    }
    if (!n_name_id_16_c) {
    }
    if (!n_len_data_8_c) {
    }
    if (!n_len_data_16_c) {
    }
    if (!n_data_8) {
    }
    if (!n_data_16) {
    }
}

int32_t dell_dvar_t::dvar_entry_t::attributes() {
    if (f_attributes)
        return m_attributes;
    f_attributes = true;
    m_attributes = 255 - attributes_c();
    return m_attributes;
}

int32_t dell_dvar_t::dvar_entry_t::flags() {
    if (f_flags)
        return m_flags;
    f_flags = true;
    m_flags = 255 - flags_c();
    return m_flags;
}

int32_t dell_dvar_t::dvar_entry_t::len_data_16() {
    if (f_len_data_16)
        return m_len_data_16;
    f_len_data_16 = true;
    m_len_data_16 = 65535 - len_data_16_c();
    return m_len_data_16;
}

int32_t dell_dvar_t::dvar_entry_t::len_data_8() {
    if (f_len_data_8)
        return m_len_data_8;
    f_len_data_8 = true;
    m_len_data_8 = 255 - len_data_8_c();
    return m_len_data_8;
}

int32_t dell_dvar_t::dvar_entry_t::name_id_16() {
    if (f_name_id_16)
        return m_name_id_16;
    f_name_id_16 = true;
    m_name_id_16 = 65535 - name_id_16_c();
    return m_name_id_16;
}

int32_t dell_dvar_t::dvar_entry_t::name_id_8() {
    if (f_name_id_8)
        return m_name_id_8;
    f_name_id_8 = true;
    m_name_id_8 = 255 - name_id_8_c();
    return m_name_id_8;
}

int32_t dell_dvar_t::dvar_entry_t::namespace_id() {
    if (f_namespace_id)
        return m_namespace_id;
    f_namespace_id = true;
    m_namespace_id = 255 - namespace_id_c();
    return m_namespace_id;
}

int32_t dell_dvar_t::dvar_entry_t::state() {
    if (f_state)
        return m_state;
    f_state = true;
    m_state = 255 - state_c();
    return m_state;
}

int32_t dell_dvar_t::dvar_entry_t::type() {
    if (f_type)
        return m_type;
    f_type = true;
    m_type = 255 - type_c();
    return m_type;
}

int8_t dell_dvar_t::data_offset() {
    if (f_data_offset)
        return m_data_offset;
    f_data_offset = true;
    m_data_offset = 9;
    return m_data_offset;
}

int32_t dell_dvar_t::flags() {
    if (f_flags)
        return m_flags;
    f_flags = true;
    m_flags = 255 - flags_c();
    return m_flags;
}

int32_t dell_dvar_t::len_store() {
    if (f_len_store)
        return m_len_store;
    f_len_store = true;
    m_len_store = 4294967295UL - len_store_c();
    return m_len_store;
}
