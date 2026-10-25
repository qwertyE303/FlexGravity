#pragma once

// Precompiled header.  The Virtools and BML SDK headers dominate the compile
// time of every translation unit, so they are parsed once here.
#include <windows.h>

#include <BML/IMod.h>
#include <BML/IBML.h>
#include <BML/ICommand.h>
#include <BML/IConfig.h>
#include <BML/ILogger.h>
#include <BML/Defines.h>
#include <BML/ExecuteBB.h>
#include <BML/ScriptHelper.h>

#include <CKAll.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
