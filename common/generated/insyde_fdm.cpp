// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

#include "insyde_fdm.h"

insyde_fdm_t::insyde_fdm_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent, insyde_fdm_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root ? p__root : this;
    m_entries = nullptr;
    m__io__raw_entries = nullptr;
    f_entries = false;
    f_valid_entry_format = false;
    f_valid_entry_size = false;
    _read();
}

void insyde_fdm_t::_read() {
    m_signature = m__io->read_u4le();
    m_store_size = m__io->read_u4le();
    m_data_offset = m__io->read_u4le();
    m_entry_size = m__io->read_u4le();
    m_entry_format = m__io->read_u1();
    m_revision = m__io->read_u1();
    m_num_extensions = m__io->read_u1();
    m_checksum = m__io->read_u1();
    m_fd_base_address = m__io->read_u8le();
}

insyde_fdm_t::~insyde_fdm_t() {
    _clean_up();
}

void insyde_fdm_t::_clean_up() {
    if (f_entries && !n_entries) {
    }
}

insyde_fdm_t::fdm_entries_t::fdm_entries_t(kaitai::kstream* p__io, insyde_fdm_t* p__parent, insyde_fdm_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    m_entries = nullptr;
    _read();
}

void insyde_fdm_t::fdm_entries_t::_read() {
    m_entries = std::unique_ptr<std::vector<std::unique_ptr<fdm_entry_t>>>(new std::vector<std::unique_ptr<fdm_entry_t>>());
    {
        int i = 0;
        while (!m__io->is_eof()) {
            m_entries->push_back(std::move(std::unique_ptr<fdm_entry_t>(new fdm_entry_t(m__io, this, m__root))));
            i++;
        }
    }
}

insyde_fdm_t::fdm_entries_t::~fdm_entries_t() {
    _clean_up();
}

void insyde_fdm_t::fdm_entries_t::_clean_up() {
}

insyde_fdm_t::fdm_entry_t::fdm_entry_t(kaitai::kstream* p__io, insyde_fdm_t::fdm_entries_t* p__parent, insyde_fdm_t* p__root) : kaitai::kstruct(p__io) {
    m__parent = p__parent;
    m__root = p__root;
    f_region_base = false;
    _read();
}

void insyde_fdm_t::fdm_entry_t::_read() {
    m_guid = m__io->read_bytes(16);
    m_region_id = m__io->read_bytes(16);
    m_region_offset = m__io->read_u8le();
    m_region_size = m__io->read_u8le();
    m_attributes = m__io->read_u4le();
    m_hash = m__io->read_bytes(32);
}

insyde_fdm_t::fdm_entry_t::~fdm_entry_t() {
    _clean_up();
}

void insyde_fdm_t::fdm_entry_t::_clean_up() {
}

int32_t insyde_fdm_t::fdm_entry_t::region_base() {
    if (f_region_base)
        return m_region_base;
    f_region_base = true;
    m_region_base = static_cast<uint32_t>(_root()->fd_base_address()) + static_cast<uint32_t>(region_offset());
    return m_region_base;
}

insyde_fdm_t::fdm_entries_t* insyde_fdm_t::entries() {
    if (f_entries)
        return m_entries.get();
    f_entries = true;
    n_entries = true;
    if ( ((entry_size() == valid_entry_size()) && (entry_format() == valid_entry_format())) ) {
        n_entries = false;
        std::streampos _pos = m__io->pos();
        m__io->seek(data_offset());
        m__raw_entries = m__io->read_bytes(store_size() - data_offset());
        m__io__raw_entries = std::unique_ptr<kaitai::kstream>(new kaitai::kstream(m__raw_entries));
        m_entries = std::unique_ptr<fdm_entries_t>(new fdm_entries_t(m__io__raw_entries.get(), this, m__root));
        m__io->seek(_pos);
    }
    return m_entries.get();
}

int8_t insyde_fdm_t::valid_entry_format() {
    if (f_valid_entry_format)
        return m_valid_entry_format;
    f_valid_entry_format = true;
    m_valid_entry_format = 0;
    return m_valid_entry_format;
}

int8_t insyde_fdm_t::valid_entry_size() {
    if (f_valid_entry_size)
        return m_valid_entry_size;
    f_valid_entry_size = true;
    m_valid_entry_size = 84;
    return m_valid_entry_size;
}
