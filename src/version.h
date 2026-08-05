#pragma once

// Single source of truth for the product version — consumed by version.rc
// (VERSIONINFO resource) and main.cpp (--version). Keep in sync with release
// tags; winget manifests reference this version.

#define DC_VERSION_MAJOR 0
#define DC_VERSION_MINOR 1
#define DC_VERSION_PATCH 0

#define DC_STR2(x) L#x
#define DC_STR(x) DC_STR2(x)
#define DC_VERSION_STRING \
    DC_STR(DC_VERSION_MAJOR) L"." DC_STR(DC_VERSION_MINOR) L"." DC_STR(DC_VERSION_PATCH)

// Narrow variants for the .rc file (RC's string handling predates wide literals).
#define DC_STR2_A(x) #x
#define DC_STR_A(x) DC_STR2_A(x)
#define DC_VERSION_STRING_A \
    DC_STR_A(DC_VERSION_MAJOR) "." DC_STR_A(DC_VERSION_MINOR) "." DC_STR_A(DC_VERSION_PATCH)
