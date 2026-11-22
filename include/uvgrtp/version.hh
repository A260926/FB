#pragma once

#include "uvgrtp/definitions.hh"
#include "uvgrtp/export.hh"

#include <cstdint>
#include <string>

namespace uvgrtp {

    /** \ingroup CORE_API
     *  @{
     */

     /**
      * \brief Get the major version number of uvgRTP
      *
      * \return Major version number.
      */
    uint16_t UVGRTP_EXPORT get_version_major();

    /**
     * \brief Get the minor version number of uvgRTP
     *
     * \return Minor version number.
     */
    uint16_t UVGRTP_EXPORT get_version_minor();

    /**
     * \brief Get the patch version number of uvgRTP
     *
     * \return Patch version number.
     */
    uint16_t UVGRTP_EXPORT get_version_patch();

    /**
     * \brief Get the full version string of uvgRTP as a C string
     *
     * \return Pointer to a null-terminated version string (lifetime: static)
     */
    const char* UVGRTP_EXPORT get_version_cstr();

    /**
     * \brief Get the Git commit hash of the uvgRTP build as a C string
     *
     * \return Pointer to a null-terminated git-hash string (lifetime: static)
     */
    const char* UVGRTP_EXPORT get_git_hash_cstr();

    /** @} */

#if UVGRTP_EXTENDED_API

    /** \ingroup EXTENDED_API
     *  @{
     */

     /**
      * \brief Get the full version string of uvgRTP
      *
      * \return Full version string (e.g., "1.2.3").
      */
    std::string get_version();

    /**
     * \brief Get the Git commit hash of the uvgRTP build
     *
     * \return Git commit hash string.
     */
    std::string get_git_hash();

    /** @} */

#endif

} // namespace uvgrtp
