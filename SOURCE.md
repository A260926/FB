# Source export
macmade/macho https://github.com/macmade/macho commit d92cd82f2952a1662784f664e50642d27e58f3f3 (MIT), lib-macho include/source and LICENSE, byte-for-byte unchanged.
Required STDXS https://github.com/macmade/STDXS commit ae4fcdda06f80ac480e375a33b01e571676bb778 (MIT), XS++ include/source and LICENSE, byte-for-byte unchanged.
The upstream Mach-O tree has .gitmodules and STDXS/xcconfig gitlinks. This self-contained export materializes STDXS at its exact gitlink; it excludes .git metadata, .gitmodules, Xcode projects, CLI display sources, and the xcconfig build-only submodule. None is used by the direct C++17 library build. STDXS UI source is retained but not linked; parser/Info/IO/String code uses only C++ standard library. No source changes are applied.
