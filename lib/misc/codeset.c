/* **********************************************************
 * Copyright 1998 VMware, Inc.  All rights reserved.
 * **********************************************************/

/*
 * codeset.c --
 *
 *    Character set and encoding conversion functions, using ICU.
 *
 *
 *      Some definitions borrow from header files from the ICU 1.8.1
 *      library.
 *
 *      ICU 1.8.1 license follows:
 *
 *      ICU License - ICU 1.8.1 and later
 *
 *      COPYRIGHT AND PERMISSION NOTICE
 *
 *      Copyright (c) 1995-2006 International Business Machines Corporation
 *      and others
 *
 *      All rights reserved.
 *
 *           Permission is hereby granted, free of charge, to any
 *      person obtaining a copy of this software and associated
 *      documentation files (the "Software"), to deal in the Software
 *      without restriction, including without limitation the rights
 *      to use, copy, modify, merge, publish, distribute, and/or sell
 *      copies of the Software, and to permit persons to whom the
 *      Software is furnished to do so, provided that the above
 *      copyright notice(s) and this permission notice appear in all
 *      copies of the Software and that both the above copyright
 *      notice(s) and this permission notice appear in supporting
 *      documentation.
 *
 *           THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
 *      KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 *      WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 *      PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN NO EVENT
 *      SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
 *      BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR
 *      CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING
 *      FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 *      CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *      OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *           Except as contained in this notice, the name of a
 *      copyright holder shall not be used in advertising or otherwise
 *      to promote the sale, use or other dealings in this Software
 *      without prior written authorization of the copyright holder.
 */

#if defined(_WIN32)
#   include <windows.h>
#   include <malloc.h>
#   include <str.h>
#else
#   define _GNU_SOURCE
#   include <string.h>
#   include <stdlib.h>
#   include <errno.h>
#   include <su.h>
#   include <sys/stat.h>
#endif

#include <stdio.h>
#include "vmware.h"
#include "vm_product.h"
#include "unicode/ucnv.h"
#include "unicode/putil.h"
#ifdef _WIN32
#include "win32u.h"
#endif
#include "file.h"
#include "util.h"
#include "codeset.h"
#include "codesetOld.h"
#include "str.h"

/*
 * Macros
 */

#define CODESET_CAN_FALLBACK_ON_NON_ICU TRUE

#if defined(__APPLE__)
#define POSIX_ICU_DIR DEFAULT_LIBDIRECTORY "/icu"
#elif !defined(WIN32)
#define POSIX_ICU_DIR "/etc/vmware/icu"
#endif

/*
 * XXX These should be passed in from the build system,
 * but I don't have time to deal with bora-vmsoft.  -- edward
 */

#define ICU_DATA_FILE "icudt38l.dat"
#ifdef _WIN32
#define ICU_DATA_FILE_DIR "%TCROOT%/noarch/icu-data-3.8-1"
#else
#define ICU_DATA_FILE_DIR "/build/toolchain/noarch/icu-data-3.8-1"
#endif

#ifdef _WIN32
#define ICU_DATA_FILE_W XCONC(L, ICU_DATA_FILE)
#define ICU_DATA_FILE_DIR_W XCONC(L, ICU_DATA_FILE_DIR)
#define ICU_DATA_FILE_PATH ICU_DATA_FILE_DIR_W DIRSEPS_W ICU_DATA_FILE_W
#else
#define ICU_DATA_FILE_PATH ICU_DATA_FILE_DIR DIRSEPS ICU_DATA_FILE
#endif


/*
 * Variables
 */

static Bool dontUseIcu = TRUE;
DEBUG_ONLY(static Bool initedIcu = FALSE;)

/*
 * Functions
 */

#ifdef _WIN32

/*
 *----------------------------------------------------------------------------
 *
 * CodeSetGetModulePath --
 *
 *      Returns the wide-character current module path. We can't use
 *      Win32U_GetModulePath because it invokes codeset.c conversion
 *      routines.
 *
 * Returns:
 *      NULL, or a utf16 string (free with free).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

utf16_t *
CodeSetGetModulePath(HANDLE hModule) // IN
{
   utf16_t *pathW = NULL;
   DWORD size = MAX_PATH;

   while (TRUE) {
      DWORD res;

      pathW = realloc(pathW, size * sizeof(wchar_t));
      if (!pathW) {
         return NULL;
      }

      res = GetModuleFileNameW(hModule, pathW, size);

      if (res == 0) {
         /* fatal error */
         goto exit;
      } else if (res == size) {
         /* buffer too small */
         size *= 2;
      } else {
         /* success */
         break;
      }
   }

  exit:
   return pathW;
}

#endif // _WIN32


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_DontUseIcu --
 *
 *    Tell codeset not to load or use ICU (or stop using it if it's
 *    already loaded). Codeset will fall back on codesetOld.c, which
 *    relies on system internationalization APIs (and may have more
 *    limited functionality). Not all APIs have a fallback, however
 *    (namely GenericToGeneric).
 *
 * Results:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    See above
 *
 *-----------------------------------------------------------------------------
 */

void
CodeSet_DontUseIcu(void)
{
   dontUseIcu = TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_Init --
 *
 *    Looks for ICU's data file in some platform-specific
 *    directory. If present, inits ICU by feeding it the path to that
 *    directory. If already inited, returns the current state (init
 *    failed/succeeded).
 *
 *    Call while single-threaded.
 *
 *    *********** WARNING ***********
 *    Do not call CodeSet_Init directly, it is called already by
 *    Unicode_Init. Lots of code depends on codeset. Please call
 *    Unicode_Init as early as possible.
 *    *******************************
 *
 * Results:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    See above
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_Init(void)
{
   DynBuf dbpath;
#ifdef _WIN32
   DWORD attribs;
   utf16_t *modPath = NULL;
   utf16_t *lastSlash;
#else
   struct stat finfo;
#endif
   char *path = NULL;
   Bool ret = FALSE;

   DynBuf_Init(&dbpath);

   DEBUG_ONLY(ASSERT(!initedIcu);)
   DEBUG_ONLY(initedIcu = TRUE;)

#ifdef USE_ICU
   /*
    * We're using system ICU, which finds its own data. So nothing to
    * do here.
    */
   dontUseIcu = FALSE;
   ret = TRUE;
   goto exit;
#endif

  /*
   * ********************* WARNING
   * Must avoid recursive calls into the codeset library here, hence
   * the idiotic hoop-jumping. DO NOT change any of these calls to
   * wrapper equivalents or call any other functions that may perform
   * string conversion.
   * ********************* WARNING
   */

#ifdef _WIN32 // {

#if vmx86_devel
   /*
    * Devel builds use toolchain directory first.
    */

   {
      WCHAR icuFilePath[MAX_PATH] = { 0 };
      DWORD n = ExpandEnvironmentStringsW(ICU_DATA_FILE_PATH,
                                          icuFilePath, ARRAYSIZE(icuFilePath));
      if (n > 0 && n < ARRAYSIZE(icuFilePath)) {
         attribs = GetFileAttributesW(icuFilePath);
         if ((INVALID_FILE_ATTRIBUTES != attribs) ||
             (attribs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            if (!CodeSetOld_Utf16leToCurrent((const char *) icuFilePath,
                                             n * sizeof *icuFilePath,
                                             &path, NULL)) {
               goto exit;
            }
            goto found;
         }
      }
   }
#endif

   /*
    * Data file must be in current module directory.
    */

   modPath = CodeSetGetModulePath(NULL);
   if (!modPath) {
      goto exit;
   }

   lastSlash = wcsrchr(modPath, DIRSEPC_W);
   if (!lastSlash) {
      goto exit;
   }

   *lastSlash = L'\0';

   if (!DynBuf_Append(&dbpath, modPath,
                      wcslen(modPath) * sizeof(utf16_t)) ||
       !DynBuf_Append(&dbpath, DIRSEPS_W,
                      wcslen(DIRSEPS_W) * sizeof(utf16_t)) ||
       !DynBuf_Append(&dbpath, ICU_DATA_FILE_W,
                      wcslen(ICU_DATA_FILE_W) * sizeof(utf16_t)) ||
       !DynBuf_Append(&dbpath, L"\0", 2)) {
      goto exit;
   }

   /*
    * Check for file existence.
    */

   attribs = GetFileAttributesW((LPCWSTR) DynBuf_Get(&dbpath));

   if ((INVALID_FILE_ATTRIBUTES == attribs) ||
       (attribs & FILE_ATTRIBUTE_DIRECTORY)) {
      goto exit;
   }

   /*
    * Convert path to local encoding using system APIs (old codeset).
    */

   if (!CodeSetOld_Utf16leToCurrent(DynBuf_Get(&dbpath),
                                    DynBuf_GetSize(&dbpath),
                                    &path, NULL)) {
      goto exit;
   }

#else // } _WIN32 {

#if vmx86_devel
   /*
    * Devel builds use toolchain directory first.
    */

   if (stat(ICU_DATA_FILE_PATH, &finfo) >= 0 && !S_ISDIR(finfo.st_mode)) {
      if ((path = strdup(ICU_DATA_FILE_PATH)) == NULL) {
	 goto exit;
      }
      goto found;
   }
#endif

   /*
    * Data file must be in POSIX_ICU_DIR.
    */

   if (!DynBuf_Append(&dbpath, POSIX_ICU_DIR, strlen(POSIX_ICU_DIR)) ||
       !DynBuf_Append(&dbpath, DIRSEPS, strlen(DIRSEPS)) ||
       !DynBuf_Append(&dbpath, ICU_DATA_FILE, strlen(ICU_DATA_FILE)) ||
       !DynBuf_Append(&dbpath, "\0", 1)) {
      goto exit;
   }

   /*
    * Check for file existence. (DO NOT CHANGE TO 'stat' WRAPPER).
    */

   path = (char *) DynBuf_Detach(&dbpath);
   if (stat(path, &finfo) < 0 || S_ISDIR(finfo.st_mode)) {
      goto exit;
   }

#endif // } _WIN32

#if vmx86_devel
found:
#endif

   /*
    * Tell ICU to use this directory.
    */
   u_setDataDirectory(path);

   dontUseIcu = FALSE;
   ret = TRUE;

  exit:
   if (!ret) {
      /*
       * There was an error initing ICU, but if we can fall back on
       * non-ICU (old CodeSet) then things are OK.
       */
      if (CODESET_CAN_FALLBACK_ON_NON_ICU) {
         ret = TRUE;
         dontUseIcu = TRUE;
      }
   }

#ifdef _WIN32
   free(modPath);
#endif

   free(path);
   DynBuf_Destroy(&dbpath);

   return ret;
}


#if defined(CURRENT_IS_UTF8)

/*
 *-----------------------------------------------------------------------------
 *
 * CodeSetDuplicateUtf8Str --
 *
 *    Duplicate UTF-8 string, appending zero terminator to its end.
 *
 * Results:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
CodeSetDuplicateUtf8Str(const char *bufIn,  // IN: Input string
                        size_t sizeIn,      // IN: Input string length
                        char **bufOut,      // OUT: "Converted" string
                        size_t *sizeOut)    // OUT: Length of string
{
   char *myBufOut;

   myBufOut = malloc(sizeIn + 1);
   if (myBufOut == NULL) {
      return FALSE;
   }

   memcpy(myBufOut, bufIn, sizeIn);
   memset(myBufOut + sizeIn, 0, 1);

   *bufOut = myBufOut;
   if (sizeOut) {
      *sizeOut = sizeIn;
   }
   return TRUE;
}

#endif // defined(CURRENT_IS_UTF8)


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSetDynBufFinalize --
 *
 *    Append NUL terminator to the buffer, and return pointer to
 *    buffer and its data size (before appending terminator). Destroys
 *    buffer on failure.
 *
 * Results:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
CodeSetDynBufFinalize(Bool ok,          // IN: Earlier steps succeeded
                      DynBuf *db,       // IN: Buffer with converted string
                      char **bufOut,    // OUT: Converted string
                      size_t *sizeOut)  // OUT: Length of string in bytes
{
   /*
    * NUL can be as long as 4 bytes if UTF-32, make no assumptions.
    */
   if (!ok || !DynBuf_Append(db, "\0\0\0\0", 4) || !DynBuf_Trim(db)) {
      DynBuf_Destroy(db);
      return FALSE;
   }

   *bufOut = DynBuf_Get(db);
   if (sizeOut) {
      *sizeOut = DynBuf_GetSize(db) - 4;
   }
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSetUtf8ToUtf16le --
 *
 *    Append the content of a buffer (that uses the UTF-8 encoding) to a
 *    DynBuf (that uses the UTF-16LE encoding)
 *
 * Results:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
CodeSetUtf8ToUtf16le(const char *bufIn,  // IN
                     size_t sizeIn,      // IN
                     DynBuf *db)         // IN
{
   return CodeSet_GenericToGenericDb("UTF-8", bufIn, sizeIn, "UTF-16LE", 0,
                                     db);
}


#if defined(__APPLE__)

/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_Utf8Normalize --
 *
 *    Calls down to CodeSetOld_Utf8Normalize.
 *
 * Results:
 *    See CodeSetOld_Utf8Normalize.
 *
 * Side effects:
 *    See CodeSetOld_Utf8Normalize.
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_Utf8Normalize(const char *bufIn,     // IN
                      size_t sizeIn,         // IN
                      Bool precomposed,      // IN
                      DynBuf *db)            // OUT
{
   return CodeSetOld_Utf8Normalize(bufIn, sizeIn, precomposed, db);
}

#endif /* defined(__APPLE__) */


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_GetCurrentCodeSet --
 *
 *    Return native code set name. Always calls down to
 *    CodeSetOld_GetCurrentCodeSet. See there for more details.
 *
 * Results:
 *    See CodeSetOld_GetCurrentCodeSet.
 *
 * Side effects:
 *    See CodeSetOld_GetCurrentCodeSet.
 *
 *-----------------------------------------------------------------------------
 */

const char *
CodeSet_GetCurrentCodeSet(void)
{
   return CodeSetOld_GetCurrentCodeSet();
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_GenericToGenericDb --
 *
 *    Append the content of a buffer (that uses the specified encoding) to a
 *    DynBuf (that uses the specified encoding).
 *
 * Results:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    String (sans NUL-termination) is appended to db.
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_GenericToGenericDb(const char *codeIn,  // IN
                           const char *bufIn,   // IN
                           size_t sizeIn,       // IN
                           const char *codeOut, // IN
                           unsigned int flags,  // IN
                           DynBuf *db)          // IN/OUT
{
   Bool result = FALSE;
   UErrorCode uerr;
   char *bufInCur;
   char *bufInEnd;
   DynBuf dbpiv;
   UChar *bufPiv;
   UChar *bufPivCur;
   UChar *bufPivEnd;
   char *bufOut;
   char *bufOutCur;
   char *bufOutEnd;
   size_t bufPivSize;
   size_t bufPivOffset;
   size_t bufOutSize;
   size_t bufOutOffset;
   UConverter *cvin = NULL;
   UConverter *cvout = NULL;
   UConverterToUCallback toUCb;
   UConverterFromUCallback fromUCb;

   ASSERT(codeIn);
   ASSERT(sizeIn == 0 || bufIn);
   ASSERT(codeOut);
   ASSERT(db);
   ASSERT((CSGTG_NORMAL == flags) || (CSGTG_TRANSLIT == flags) ||
          (CSGTG_IGNORE == flags));

   if (dontUseIcu) {
      /*
       * Fall back.
       */
      return CodeSetOld_GenericToGenericDb(codeIn, bufIn, sizeIn, codeOut,
                                           flags, db);
   }

   DynBuf_Init(&dbpiv);

   /*
    * Trivial case.
    */
   if ((0 == sizeIn) || (NULL == bufIn)) {
      result = TRUE;
      goto exit;
   }

   /*
    * Open converters.
    */
   uerr = U_ZERO_ERROR;
   cvin = ucnv_open(codeIn, &uerr);
   if (!cvin) {
      goto exit;
   }

   uerr = U_ZERO_ERROR;
   cvout = ucnv_open(codeOut, &uerr);
   if (!cvout) {
      goto exit;
   }

   /*
    * Set callbacks according to flags.
    */
   switch (flags) {
   case CSGTG_NORMAL:
      toUCb = UCNV_TO_U_CALLBACK_STOP;
      fromUCb = UCNV_FROM_U_CALLBACK_STOP;
      break;

   case CSGTG_TRANSLIT:
      toUCb = UCNV_TO_U_CALLBACK_SUBSTITUTE;
      fromUCb = UCNV_FROM_U_CALLBACK_SUBSTITUTE;
      break;

   case CSGTG_IGNORE:
      toUCb = UCNV_TO_U_CALLBACK_SKIP;
      fromUCb = UCNV_FROM_U_CALLBACK_SKIP;
      break;

   default:
      NOT_IMPLEMENTED();
      break;
   }

   uerr = U_ZERO_ERROR;
   ucnv_setToUCallBack(cvin, toUCb, NULL, NULL, NULL, &uerr);
   if (U_ZERO_ERROR != uerr) {
      goto exit;
   }

   uerr = U_ZERO_ERROR;
   ucnv_setFromUCallBack(cvout, fromUCb, NULL, NULL, NULL, &uerr);
   if (U_ZERO_ERROR != uerr) {
      goto exit;
   }

   /*
    * Convert to pivot buffer. As a starting guess, allocate a pivot
    * buffer the size of the input string times UChar size (with a
    * fudge constant added in to avoid degen cases).
    */
   bufInCur = (char *) bufIn;
   bufInEnd = (char *) bufIn + sizeIn;
   bufPivSize = (sizeIn + 4) * sizeof(UChar);
   bufPivOffset = 0;

   while (TRUE) {
      if (!DynBuf_Enlarge(&dbpiv, bufPivSize * sizeof(UChar))) {
         goto exit;
      }

      bufPiv = (UChar *) DynBuf_Get(&dbpiv);
      bufPivCur = bufPiv + bufPivOffset;
      bufPivEnd = bufPiv + bufPivSize;

      uerr = U_ZERO_ERROR;
      ucnv_toUnicode(cvin, &bufPivCur, bufPivEnd, (const char **) &bufInCur,
                     bufInEnd, NULL, TRUE, &uerr);

      if (U_BUFFER_OVERFLOW_ERROR == uerr) {
         /*
          * 'bufInCur' points to the next chunk of input string to
          * convert, so we leave it alone. 'bufPivCur' points to right
          * after the last UChar written, so it should be at or almost
          * at the end of the buffer.
          *
          * Our guess at 'bufPivSize' was obviously wrong, just double
          * the buffer.
          */
         bufPivSize *= 2;
         bufPivOffset = bufPivCur - bufPiv;
      } else if (U_FAILURE(uerr)) {
         /*
          * Failure.
          */
         goto exit;
      } else {
         /*
          * Success.
          */
         break;
      }
   }

   /*
    * Convert from pivot buffer. Since we're probably most likely
    * converting to UTF-8, a safe guess for the byte size of the
    * output buffer would be the same number of code units as in the
    * pivot buffer (with a fudge constant added to avoid degen cases).
    */
   bufPivEnd = bufPivCur;
   bufPivCur = bufPiv;
   bufPivSize = bufPivEnd - bufPivCur;
   bufOutSize = bufPivSize + 4;
   bufOutOffset = 0;

   while (TRUE) {
      if (!DynBuf_Enlarge(db, bufOutSize)) {
         goto exit;
      }

      bufOut = (char *) DynBuf_Get(db);
      bufOutCur = bufOut + bufOutOffset;
      bufOutEnd = bufOut + bufOutSize;

      uerr = U_ZERO_ERROR;
      ucnv_fromUnicode(cvout, &bufOutCur, bufOutEnd,
                       (const UChar **) &bufPivCur, bufPivEnd, NULL, TRUE,
                       &uerr);

      if (U_BUFFER_OVERFLOW_ERROR == uerr) {
         /*
          * 'bufPivCur' points to the next chunk of pivot string to
          * convert, so we leave it alone. 'bufOutCur' points to right
          * after the last unit written, so it should be at or almost
          * at the end of the buffer.
          *
          * Our guess at 'bufOutSize' was obviously wrong, just double
          * the buffer.
          */
         bufOutSize *= 2;
         bufOutOffset = bufOutCur - bufOut;
      } else if (U_FAILURE(uerr)) {
         /*
          * Failure.
          */
         goto exit;
      } else {
         /*
          * "This was a triumph.
          *  I'm making a note here:
          *  HUGE SUCCESS.
          *  It's hard to overstate
          *  my satisfaction."
          */
         break;
      }
   }

   /*
    * Set final size and return.
    */
   DynBuf_SetSize(db, bufOutCur - bufOut);

   result = TRUE;

  exit:
   DynBuf_Destroy(&dbpiv);

   if (cvin) {
      ucnv_close(cvin);
   }

   if (cvout) {
      ucnv_close(cvout);
   }

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_GenericToGeneric --
 *
 *    Non-db version of CodeSet_GenericToGenericDb.
 *
 * Results:
 *    TRUE on success, plus allocated string
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_GenericToGeneric(const char *codeIn,  // IN
                         const char *bufIn,   // IN
                         size_t sizeIn,       // IN
                         const char *codeOut, // IN
                         unsigned int flags,  // IN
                         char **bufOut,       // OUT
                         size_t *sizeOut)     // OUT
{
   DynBuf db;
   Bool ok;

   DynBuf_Init(&db);
   ok = CodeSet_GenericToGenericDb(codeIn, bufIn, sizeIn, codeOut, flags, &db);
   return CodeSetDynBufFinalize(ok, &db, bufOut, sizeOut);
}


/*
 * Generic remarks: here is my understanding of those terms as of 2001/12/27:
 *
 * BOM
 *    Byte-Order Mark
 *
 * BMP
 *    Basic Multilingual Plane. This plane comprises the first 2^16 code
 *    positions of ISO/IEC 10646's canonical code space
 *
 * UCS
 *    Universal Character Set. Encoding form specified by ISO/IEC 10646
 *
 * UCS-2
 *    Directly store all Unicode scalar value (code point) from U+0000 to
 *    U+FFFF on 2 bytes. Consequently, this representation can only hold
 *    characters in the BMP
 *
 * UCS-4
 *    Directly store a Unicode scalar value (code point) from U-00000000 to
 *    U-FFFFFFFF on 4 bytes
 *
 * UTF
 *    Abbreviation for Unicode (or UCS) Transformation Format
 *
 * UTF-8
 *    Unicode (or UCS) Transformation Format, 8-bit encoding form. UTF-8 is the
 *    Unicode Transformation Format that serializes a Unicode scalar value
 *    (code point) as a sequence of 1 to 6 bytes
 *
 * UTF-16
 *    UCS-2 + surrogate mechanism: allow to encode some non-BMP Unicode
 *    characters in a UCS-2 string, by using 2 2-byte units. See the Unicode
 *    standard, v2.0
 *
 * UTF-32
 *    Directly store all Unicode scalar value (code point) from U-00000000 to
 *    U-0010FFFF on 4 bytes. This is a subset of UCS-4
 *
 *   --hpreg
 */


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_Utf8ToCurrent --
 *
 *    Convert the content of a buffer (that uses the UTF-8 encoding) into
 *    another buffer (that uses the current encoding).
 *
 * Results:
 *    TRUE on success: '*bufOut' contains the allocated, NUL terminated buffer.
 *                     If not NULL, '*sizeOut' contains the size of the buffer
 *                     (excluding the NUL terminator)
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_Utf8ToCurrent(const char *bufIn,  // IN
                      size_t sizeIn,      // IN
                      char **bufOut,      // OUT
                      size_t *sizeOut)    // OUT
{
#if !defined(CURRENT_IS_UTF8)
   DynBuf db;
   Bool ok;
#endif

   /*
    * Fallback if necessary.
    */
   if (dontUseIcu) {
      return CodeSetOld_Utf8ToCurrent(bufIn, sizeIn, bufOut, sizeOut);
   }

#if defined(CURRENT_IS_UTF8)
   return CodeSetDuplicateUtf8Str(bufIn, sizeIn, bufOut, sizeOut);
#else
   DynBuf_Init(&db);
   ok = CodeSet_GenericToGenericDb("UTF-8", bufIn, sizeIn,
                                   CodeSet_GetCurrentCodeSet(), 0, &db);
   return CodeSetDynBufFinalize(ok, &db, bufOut, sizeOut);
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_CurrentToUtf8 --
 *
 *    Convert the content of a buffer (that uses the current encoding) into
 *    another buffer (that uses the UTF-8 encoding).
 *
 * Results:
 *    TRUE on success: '*bufOut' contains the allocated, NUL terminated buffer.
 *                     If not NULL, '*sizeOut' contains the size of the buffer
 *                     (excluding the NUL terminator)
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_CurrentToUtf8(const char *bufIn,  // IN
                      size_t sizeIn,      // IN
                      char **bufOut,      // OUT
                      size_t *sizeOut)    // OUT
{
#if !defined(CURRENT_IS_UTF8)
   DynBuf db;
   Bool ok;
#endif

   /*
    * Fallback if necessary.
    */
   if (dontUseIcu) {
      return CodeSetOld_CurrentToUtf8(bufIn, sizeIn, bufOut, sizeOut);
   }

#if defined(CURRENT_IS_UTF8)
   return CodeSetDuplicateUtf8Str(bufIn, sizeIn, bufOut, sizeOut);
#else
   DynBuf_Init(&db);
   ok = CodeSet_GenericToGenericDb(CodeSet_GetCurrentCodeSet(), bufIn, sizeIn,
                                   "UTF-8", 0, &db);
   return CodeSetDynBufFinalize(ok, &db, bufOut, sizeOut);
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_Utf16leToUtf8_Db --
 *
 *    Append the content of a buffer (that uses the UTF-16LE encoding) to a
 *    DynBuf (that uses the UTF-8 encoding).
 *
 * Results:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_Utf16leToUtf8_Db(const char *bufIn, // IN
                         size_t sizeIn,     // IN
                         DynBuf *db)        // IN
{
   /*
    * Fallback if necessary.
    */
   if (dontUseIcu) {
      return CodeSetOld_Utf16leToUtf8_Db(bufIn, sizeIn, db);
   }

   return CodeSet_GenericToGenericDb("UTF-16LE", bufIn, sizeIn, "UTF-8", 0,
                                     db);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_Utf16leToUtf8 --
 *
 *    Convert the content of a buffer (that uses the UTF-16LE encoding) into
 *    another buffer (that uses the UTF-8 encoding).
 *
 *    The operation is inversible (its inverse is CodeSet_Utf8ToUtf16le).
 *
 * Results:
 *    TRUE on success: '*bufOut' contains the allocated, NUL terminated buffer.
 *                     If not NULL, '*sizeOut' contains the size of the buffer
 *                     (excluding the NUL terminator)
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_Utf16leToUtf8(const char *bufIn,  // IN
                      size_t sizeIn,      // IN
                      char **bufOut,      // OUT
                      size_t *sizeOut)    // OUT
{
   DynBuf db;
   Bool ok;

   /*
    * Fallback if necessary.
    */
   if (dontUseIcu) {
      return CodeSetOld_Utf16leToUtf8(bufIn, sizeIn, bufOut, sizeOut);
   }

   DynBuf_Init(&db);
   ok = CodeSet_Utf16leToUtf8_Db(bufIn, sizeIn, &db);
   return CodeSetDynBufFinalize(ok, &db, bufOut, sizeOut);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_Utf8ToUtf16le --
 *
 *    Convert the content of a buffer (that uses the UTF-8 encoding) into
 *    another buffer (that uses the UTF-16LE encoding).
 *
 *    The operation is inversible (its inverse is CodeSet_Utf16leToUtf8).
 *
 * Results:
 *    TRUE on success: '*bufOut' contains the allocated, NUL terminated buffer.
 *                     If not NULL, '*sizeOut' contains the size of the buffer
 *                     (excluding the NUL terminator)
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_Utf8ToUtf16le(const char *bufIn,  // IN
                      size_t sizeIn,      // IN
                      char **bufOut,      // OUT
                      size_t *sizeOut)    // OUT
{
   DynBuf db;
   Bool ok;

   /*
    * Fallback if necessary.
    */
   if (dontUseIcu) {
      return CodeSetOld_Utf8ToUtf16le(bufIn, sizeIn, bufOut, sizeOut);
   }

   DynBuf_Init(&db);
   ok = CodeSetUtf8ToUtf16le(bufIn, sizeIn, &db);
   return CodeSetDynBufFinalize(ok, &db, bufOut, sizeOut);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_Utf8FormDToUtf8FormC  --
 *
 *    Convert the content of a buffer (that uses the UTF-8 encoding) 
 *    which is in normal form D (decomposed) into another buffer
 *    (that uses the UTF-8 encoding) and is normalized as
 *    precomposed (Normalization Form C).
 *
 * Results:
 *    TRUE on success: '*bufOut' contains a NUL terminated buffer.
 *                     If not NULL, '*sizeOut' contains the size of the buffer
 *                     (excluding the NUL terminator)
 *    FALSE on failure
 *
 * Side effects:
 *    '*bufOut' contains the allocated, NUL terminated buffer.
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_Utf8FormDToUtf8FormC(const char *bufIn,     // IN
                             size_t sizeIn,         // IN
                             char **bufOut,         // OUT
                             size_t *sizeOut)       // OUT
{
   /*
    * Fallback if necessary.
    */
   if (dontUseIcu) {
      return CodeSetOld_Utf8FormDToUtf8FormC(bufIn, sizeIn, bufOut, sizeOut);
   }

#if defined(__APPLE__)
   DynBuf db;
   Bool ok;
   DynBuf_Init(&db);
   ok = CodeSet_Utf8Normalize(bufIn, sizeIn, TRUE, &db);
   return CodeSetDynBufFinalize(ok, &db, bufOut, sizeOut);
#else
   NOT_IMPLEMENTED();
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_Utf8FormCToUtf8FormD  --
 *
 *    Convert the content of a buffer (that uses the UTF-8 encoding) 
 *    which is in normal form C (precomposed) into another buffer
 *    (that uses the UTF-8 encoding) and is normalized as
 *    decomposed (Normalization Form D).
 *
 * Results:
 *    TRUE on success: '*bufOut' contains a NUL terminated buffer.
 *                     If not NULL, '*sizeOut' contains the size of the buffer
 *                     (excluding the NUL terminator)
 *    FALSE on failure
 *
 * Side effects:
 *    '*bufOut' contains the allocated, NUL terminated buffer.
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_Utf8FormCToUtf8FormD(const char *bufIn,     // IN
                             size_t sizeIn,         // IN
                             char **bufOut,         // OUT
                             size_t *sizeOut)       // OUT
{
   /*
    * Fallback if necessary.
    */
   if (dontUseIcu) {
      return CodeSetOld_Utf8FormCToUtf8FormD(bufIn, sizeIn, bufOut, sizeOut);
   }

#if defined(__APPLE__)
   DynBuf db;
   Bool ok;
   DynBuf_Init(&db);
   ok = CodeSet_Utf8Normalize(bufIn, sizeIn, FALSE, &db);
   return CodeSetDynBufFinalize(ok, &db, bufOut, sizeOut);
#else
   NOT_IMPLEMENTED();
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_CurrentToUtf16le --
 *
 *    Convert the content of a buffer (that uses the current encoding) into
 *    another buffer (that uses the UTF-16LE encoding).
 *
 * Results:
 *    TRUE on success: '*bufOut' contains the allocated, NUL terminated buffer.
 *                     If not NULL, '*sizeOut' contains the size of the buffer
 *                     (excluding the NUL terminator)
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_CurrentToUtf16le(const char *bufIn, // IN
                         size_t sizeIn,     // IN
                         char **bufOut,     // OUT
                         size_t *sizeOut)   // OUT
{
   DynBuf db;
   Bool ok;

   /*
    * Fallback if necessary.
    */
   if (dontUseIcu) {
      return CodeSetOld_CurrentToUtf16le(bufIn, sizeIn, bufOut, sizeOut);
   }

   DynBuf_Init(&db);
   ok = CodeSet_GenericToGenericDb(CodeSet_GetCurrentCodeSet(), bufIn, sizeIn,
                                   "UTF-16LE", 0, &db);
   return CodeSetDynBufFinalize(ok, &db, bufOut, sizeOut);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_Utf16leToCurrent --
 *
 *    Convert the content of a buffer (that uses the UTF-16 little endian
 *    encoding) into another buffer (that uses the current encoding)
 *
 * Results:
 *    TRUE on success: '*bufOut' contains the allocated, NUL terminated buffer.
 *                     If not NULL, '*sizeOut' contains the size of the buffer
 *                     (excluding the NUL terminator)
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_Utf16leToCurrent(const char *bufIn,  // IN
                         size_t sizeIn,      // IN
                         char **bufOut,      // OUT
                         size_t *sizeOut)    // OUT
{
   DynBuf db;
   Bool ok;

   /*
    * Fallback if necessary.
    */
   if (dontUseIcu) {
      return CodeSetOld_Utf16leToCurrent(bufIn, sizeIn, bufOut, sizeOut);
   }

   DynBuf_Init(&db);
   ok = CodeSet_GenericToGenericDb("UTF-16LE", bufIn, sizeIn,
                                   CodeSet_GetCurrentCodeSet(), 0, &db);
   return CodeSetDynBufFinalize(ok, &db, bufOut, sizeOut);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_Utf16beToCurrent --
 *
 *    Convert the content of a buffer (that uses the UTF-16 big endian
 *    encoding) into another buffer (that uses the current encoding)
 *
 * Results:
 *    TRUE on success: '*bufOut' contains the allocated, NUL terminated buffer.
 *                     If not NULL, '*sizeOut' contains the size of the buffer
 *                     (excluding the NUL terminator)
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_Utf16beToCurrent(const char *bufIn,  // IN
                         size_t sizeIn,      // IN
                         char **bufOut,      // OUT
                         size_t *sizeOut)    // OUT
{
   DynBuf db;
   Bool ok;

   /*
    * Fallback if necessary.
    */
   if (dontUseIcu) {
      return CodeSetOld_Utf16beToCurrent(bufIn, sizeIn, bufOut, sizeOut);
   }

   DynBuf_Init(&db);
   ok = CodeSet_GenericToGenericDb("UTF-16BE", bufIn, sizeIn,
                                   CodeSet_GetCurrentCodeSet(), 0, &db);
   return CodeSetDynBufFinalize(ok, &db, bufOut, sizeOut);
}


/*
 *-----------------------------------------------------------------------------
 *
 * CodeSet_IsEncodingSupported --
 *
 *    Ask ICU if it supports the specific encoding.
 *
 * Results:
 *    TRUE on success
 *    FALSE on failure
 *
 * Side effects:
 *    None
 *
 *-----------------------------------------------------------------------------
 */

Bool
CodeSet_IsEncodingSupported(const char *name) // IN
{
   UConverter *cv;
   UErrorCode uerr;

   /*
    * Fallback if necessary.
    */
   if (dontUseIcu) {
      return CodeSetOld_IsEncodingSupported(name);
   }

   /*
    * Try to open the encoding.
    */
   uerr = U_ZERO_ERROR;
   cv = ucnv_open(name, &uerr);
   if (cv) {
      ucnv_close(cv);
      return TRUE;
   }

   return FALSE;
}
