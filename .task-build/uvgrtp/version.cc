#include "uvgrtp/version.hh"

#include <cstdint>
#include <string>

namespace uvgrtp {

uint16_t get_version_major() { return 4; }

uint16_t get_version_minor() { return 0; }

uint16_t get_version_patch() { return 1; }


#ifdef RTP_RELEASE_COMMIT
static const char* uvgrtp_version_cstr = "4.0.1-release";
#else
static const char* uvgrtp_version_cstr = "4.0.1-source";
#endif

static const char* uvgrtp_git_hash_cstr = "source";

const char* get_version_cstr() { return uvgrtp_version_cstr; }

const char* get_git_hash_cstr() { return uvgrtp_git_hash_cstr; }

#if UVGRTP_EXTENDED_API
#ifdef RTP_RELEASE_COMMIT
    std::string get_version() { return "4.0.1-release"; }
#else
    std::string get_version() { return "4.0.1-source"; }
#endif

std::string get_git_hash() {return "source";}
#endif


} // namespace uvgrtp
