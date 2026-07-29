#pragma once

#if defined(_WIN32)
#  if defined(MFX50_DLL_BUILD) || defined(MFX50_ENCODER_DLL_BUILD)
#    define MFX50_API __declspec(dllexport)
#  else
#    define MFX50_API __declspec(dllimport)
#  endif
#else
#  define MFX50_API
#endif
