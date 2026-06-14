#pragma once

/*
 * Local static-build export shim for vendored miniz 3.1.1.
 *
 * The upstream CMake build generates this header. SuperZip vendors the
 * amalgamated sources directly, so static symbols do not need import/export
 * annotations.
 */

#define MINIZ_EXPORT
