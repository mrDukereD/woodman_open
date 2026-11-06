#pragma once

#ifndef MINIZ_EXPORT
  #if defined(_WIN32) && defined(MINIZ_BUILD_DLL)
    #define MINIZ_EXPORT __declspec(dllexport)
  #elif defined(_WIN32) && defined(MINIZ_USE_DLL)
    #define MINIZ_EXPORT __declspec(dllimport)
  #else
    #define MINIZ_EXPORT
  #endif
#endif
