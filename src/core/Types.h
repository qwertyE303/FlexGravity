#pragma once

namespace fg::core {

// Virtools' CK_ID is `unsigned long`.  Core keeps its own alias so it never
// has to include an engine header; FlexGravity.cpp static_asserts that the two
// types are the same.
using ObjectId = unsigned long;

} // namespace fg::core
