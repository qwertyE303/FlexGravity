#pragma once

#include <string>

#include <CKAll.h>

namespace fg::vt {

// Text helpers shared by every diagnostic.
std::string SafeName(CKObject *object);
std::string IdText(CKObject *object);
std::string ClassText(CKObject *object);
std::string ObjectText(CKObject *object);

// FG_CField / FG_CRegion objects are named "\x01__<name>" (three bytes: 0x01,
// '_', '_').  The prefix is mandatory: a row whose object lacks it is invalid.
bool HasPlaceholderPrefix(const std::string &raw);
std::string StripPlaceholderPrefix(const std::string &raw);
// Prefix stripped and ANSI/GBK converted to UTF-8 for the log; control bytes
// are replaced so the log never receives a raw 0x01.
std::string DisplayName(const std::string &raw);
std::string DisplayObjectName(CKObject *object);

const char *ArrayTypeName(CK_ARRAYTYPE type);

// The Virtools 2.1 SDK headers are older than the engine shipped with
// Ballance, so every virtual call into CKCurve is guarded.  A failed probe
// must never take the game down.  These wrappers contain no objects with
// destructors, which is required for __try (MSVC C2712).
int SafeCurvePointCount(CKCurve *curve);
CKCurvePoint *SafeCurveControlPoint(CKCurve *curve, int index);
CKCurve *SafeCurveCast(CK3dEntity *entity);
bool SafeWorldPosition(CK3dEntity *entity, VxVector &out);
int SafeCurveIsOpen(CKCurve *curve);

} // namespace fg::vt
