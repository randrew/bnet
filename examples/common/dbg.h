/*
 * Copyright 2011-2016 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bnet#license-bsd-2-clause
 */

#ifndef __DBG_H__
#define __DBG_H__

#include <stdarg.h> // va_list
#include <stdint.h>

#define DBG_STRINGIZE(_x) DBG_STRINGIZE_(_x)
#define DBG_STRINGIZE_(_x) #_x
#define DBG_FILE_LINE_LITERAL "" __FILE__ "(" DBG_STRINGIZE(__LINE__) "): "
#define DBG(_format, ...) dbgPrintf(DBG_FILE_LINE_LITERAL "" _format "\n", ##__VA_ARGS__)

extern void dbgPrintfVargs(const char* _format, va_list _argList);
extern void dbgPrintf(const char* _format, ...);
extern void dbgPrintfData(const void* _data, uint32_t _size, const char* _format, ...);

#endif // __DBG_H__
