#pragma once

// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

class dell_dvar_t;

#include "../kaitai/kaitaistruct.h"
#include <stdint.h>
#include <memory>
#include <vector>

#if KAITAI_STRUCT_VERSION < 11000L
#error "Incompatible Kaitai Struct C++/STL API: version 0.11 or later is required"
#endif

class dell_dvar_t : public kaitai::kstruct {

public:
    class dvar_entry_t;

    dell_dvar_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, dell_dvar_t* p__root = nullptr);

private:
    void _read();
    void _clean_up();

public:
    ~dell_dvar_t();

    class dvar_entry_t : public kaitai::kstruct {

    public:

        dvar_entry_t(kaitai::kstream* p__io, dell_dvar_t* p__parent = nullptr, dell_dvar_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~dvar_entry_t();

    private:
        bool f_attributes;
        int32_t m_attributes;

    public:
        int32_t attributes();

    private:
        bool f_flags;
        int32_t m_flags;

    public:
        int32_t flags();

    private:
        bool f_len_data_16;
        int32_t m_len_data_16;

    public:
        int32_t len_data_16();

    private:
        bool f_len_data_8;
        int32_t m_len_data_8;

    public:
        int32_t len_data_8();

    private:
        bool f_name_id_16;
        int32_t m_name_id_16;

    public:
        int32_t name_id_16();

    private:
        bool f_name_id_8;
        int32_t m_name_id_8;

    public:
        int32_t name_id_8();

    private:
        bool f_namespace_id;
        int32_t m_namespace_id;

    public:
        int32_t namespace_id();

    private:
        bool f_state;
        int32_t m_state;

    public:
        int32_t state();

    private:
        bool f_type;
        int32_t m_type;

    public:
        int32_t type();

    private:
        uint8_t m_state_c;
        uint8_t m_flags_c;
        bool n_flags_c;

    public:
        bool _is_null_flags_c() { flags_c(); return n_flags_c; };

    private:
        uint8_t m_type_c;
        bool n_type_c;

    public:
        bool _is_null_type_c() { type_c(); return n_type_c; };

    private:
        uint8_t m_attributes_c;
        bool n_attributes_c;

    public:
        bool _is_null_attributes_c() { attributes_c(); return n_attributes_c; };

    private:
        uint8_t m_namespace_id_c;
        bool n_namespace_id_c;

    public:
        bool _is_null_namespace_id_c() { namespace_id_c(); return n_namespace_id_c; };

    private:
        std::string m_namespace_guid;
        bool n_namespace_guid;

    public:
        bool _is_null_namespace_guid() { namespace_guid(); return n_namespace_guid; };

    private:
        uint8_t m_name_id_8_c;
        bool n_name_id_8_c;

    public:
        bool _is_null_name_id_8_c() { name_id_8_c(); return n_name_id_8_c; };

    private:
        uint16_t m_name_id_16_c;
        bool n_name_id_16_c;

    public:
        bool _is_null_name_id_16_c() { name_id_16_c(); return n_name_id_16_c; };

    private:
        uint8_t m_len_data_8_c;
        bool n_len_data_8_c;

    public:
        bool _is_null_len_data_8_c() { len_data_8_c(); return n_len_data_8_c; };

    private:
        uint16_t m_len_data_16_c;
        bool n_len_data_16_c;

    public:
        bool _is_null_len_data_16_c() { len_data_16_c(); return n_len_data_16_c; };

    private:
        std::string m_data_8;
        bool n_data_8;

    public:
        bool _is_null_data_8() { data_8(); return n_data_8; };

    private:
        std::string m_data_16;
        bool n_data_16;

    public:
        bool _is_null_data_16() { data_16(); return n_data_16; };

    private:
        dell_dvar_t* m__root;
        dell_dvar_t* m__parent;

    public:
        uint8_t state_c() const { return m_state_c; }
        uint8_t flags_c() const { return m_flags_c; }
        uint8_t type_c() const { return m_type_c; }
        uint8_t attributes_c() const { return m_attributes_c; }
        uint8_t namespace_id_c() const { return m_namespace_id_c; }
        std::string namespace_guid() const { return m_namespace_guid; }
        uint8_t name_id_8_c() const { return m_name_id_8_c; }
        uint16_t name_id_16_c() const { return m_name_id_16_c; }
        uint8_t len_data_8_c() const { return m_len_data_8_c; }
        uint16_t len_data_16_c() const { return m_len_data_16_c; }
        std::string data_8() const { return m_data_8; }
        std::string data_16() const { return m_data_16; }
        dell_dvar_t* _root() const { return m__root; }
        dell_dvar_t* _parent() const { return m__parent; }
    };

private:
    bool f_data_offset;
    int8_t m_data_offset;

public:
    int8_t data_offset();

private:
    bool f_flags;
    int32_t m_flags;

public:
    int32_t flags();

private:
    bool f_len_store;
    int32_t m_len_store;

public:
    int32_t len_store();

private:
    std::string m_signature;
    uint32_t m_len_store_c;
    uint8_t m_flags_c;
    std::unique_ptr<std::vector<std::unique_ptr<dvar_entry_t>>> m_entries;
    dell_dvar_t* m__root;
    kaitai::kstruct* m__parent;

public:
    std::string signature() const { return m_signature; }
    uint32_t len_store_c() const { return m_len_store_c; }
    uint8_t flags_c() const { return m_flags_c; }
    std::vector<std::unique_ptr<dvar_entry_t>>* entries() const { return m_entries.get(); }
    dell_dvar_t* _root() const { return m__root; }
    kaitai::kstruct* _parent() const { return m__parent; }
};
