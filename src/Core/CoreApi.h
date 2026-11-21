#pragma once
// H-4 spike: data that lives in TartarusCore.dll. Functions are exported by
// WINDOWS_EXPORT_ALL_SYMBOLS; global/static DATA must be dllimport'ed by the
// modules that read it, so mark every exported variable with TARTARUS_CORE_API.
#if defined(_WIN32) && defined(TARTARUS_CORE_DLL_ON)
#  if defined(TARTARUS_CORE_BUILD)
#    define TARTARUS_CORE_API __declspec(dllexport)
#  else
#    define TARTARUS_CORE_API __declspec(dllimport)
#  endif
#else
#  define TARTARUS_CORE_API
#endif
