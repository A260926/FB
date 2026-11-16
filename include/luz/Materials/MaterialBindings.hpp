#pragma once

#include "Materials/Material.hpp"
#include <map>
#include <memory>
#include <string>

// Material slot bindings used by OBJ meshes. Keys are the slot names declared by
// "usemtl" lines (case-sensitive, empty string for faces without one).
using MaterialBindings = std::map<std::string, std::shared_ptr<Material>>;
