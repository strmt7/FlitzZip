# Purpose: Give all packed FSE construction/state consumers one checked layout.
# Inputs: Verified predecessor text and component. Outputs: Proposed source with
# unchanged packed ABI and explicit region/count contracts.
function(superzip_rewrite_zstd_fse_table content part output)
  if(part STREQUAL fse_header)
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ZstdFseTableGeometry.c"
         helper)
    string(CONCAT _zstd_emission_0
                  "MEM_STATIC void FSE_initCState(FSE_CState_t* "
                  "statePtr, const FSE_CTable* ct)")
    string(
      CONCAT _zstd_emission_1
             "${helper}\n/* Purpose: Initialize compression"
             " state from the checked packed regions.\n * I"
             "nputs: Successfully constructed live FSE tabl"
             "e. Outputs: Borrowed state/transform regions "
             "and initial state. */\nMEM_STATIC void FSE_in"
             "itCState(FSE_CState_t* statePtr, const FSE_CT"
             "able* ct)")
    string(REPLACE "${_zstd_emission_0}" "${_zstd_emission_1}" content
                   "${content}")
    string(REPLACE "${_zstd_fse_state_original}" "${_zstd_fse_state_checked}"
                   content "${content}")
  elseif(part STREQUAL fse_builder)
    string(
      CONCAT _zstd_emission_2
             "    FSE_CTableGeometry const geometry = FSE_g"
             "etCTableGeometry(tableLog, maxSymbolValue);\n"
             "    if (geometry.stateCount == 0) return ERRO"
             "R(tableLog_tooLarge);\n    U32 const tableSiz"
             "e = 1U << tableLog;")
    string(REPLACE "    U32 const tableSize = 1 << tableLog;"
                   "${_zstd_emission_2}" content "${content}")
    string(CONCAT _zstd_emission_3
                  "    U16* const tableU16 = (U16*)((BYTE*)ptr +"
                  " sizeof(FSE_CTableHeader));")
    string(REPLACE "    U16* const tableU16 = ( (U16*) ptr) + 2;"
                   "${_zstd_emission_3}" content "${content}")
    string(CONCAT _zstd_emission_4
                  "    void* const FSCT = ((U32*)ptr) + 1 /* hea"
                  "der */ + (tableLog ? tableSize>>1 : 1) ;")
    string(
      REPLACE "${_zstd_emission_4}"
              "    void* const FSCT = (BYTE*)ptr + geometry.transformOffset;"
              content "${content}")
    string(
      CONCAT _zstd_emission_5 "    FSE_CTableGeometry const geometry = FSE_g"
             "etCTableGeometry(0, symbolValue);\n    U16* t"
             "ableU16 = (U16*)((BYTE*)ptr + sizeof(FSE_CTab" "leHeader));")
    string(REPLACE "    U16* tableU16 = ( (U16*) ptr) + 2;"
                   "${_zstd_emission_5}" content "${content}")
    string(REPLACE "    void* FSCTptr = (U32*)ptr + 2;"
                   "    void* FSCTptr = (BYTE*)ptr + geometry.transformOffset;"
                   content "${content}")
    string(CONCAT _zstd_emission_6
                  "MEM_write16((BYTE*)ptr + offsetof(FSE_CTableH"
                  "eader, tableLog), (U16)tableLog);")
    string(REPLACE "tableU16[-2] = (U16) tableLog;" "${_zstd_emission_6}"
                   content "${content}")
    string(CONCAT _zstd_emission_7
                  "MEM_write16((BYTE*)ptr + offsetof(FSE_CTableH"
                  "eader, maxSymbolValue), (U16)maxSymbolValue);")
    string(REPLACE "tableU16[-1] = (U16) maxSymbolValue;" "${_zstd_emission_7}"
                   content "${content}")
    string(
      REPLACE
        "tableU16[-2] = (U16) 0;"
        "MEM_write16((BYTE*)ptr + offsetof(FSE_CTableHeader, tableLog), 0);"
        content "${content}")
    string(CONCAT _zstd_emission_8
                  "MEM_write16((BYTE*)ptr + offsetof(FSE_CTableH"
                  "eader, maxSymbolValue), symbolValue);")
    string(REPLACE "tableU16[-1] = (U16) symbolValue;" "${_zstd_emission_8}"
                   content "${content}")
  else()
    string(REPLACE "${_zstd_fse_max_original}" "${_zstd_fse_max_checked}"
                   content "${content}")
  endif()
  set(${output}
      "${content}"
      PARENT_SCOPE)
endfunction()

set(_zstd_fse_state_original
    [=[    const void* ptr = ct;
    const U16* u16ptr = (const U16*) ptr;
    const U32 tableLog = MEM_read16(ptr);
    statePtr->value = (ptrdiff_t)1<<tableLog;
    statePtr->stateTable = u16ptr+2;
    statePtr->symbolTT = ct + 1 + (tableLog ? (1<<(tableLog-1)) : 1);
    statePtr->stateLog = tableLog;]=])
string(
  CONCAT _zstd_fse_state_checked
         "    FSE_CTableHeader const header = FSE_readC"
         "TableHeader(ct);\n"
         "    FSE_CTableGeometry const geometry = FSE_g"
         "etCTableGeometry(header.tableLog, header.maxS"
         "ymbolValue);\n"
         "    const BYTE* const bytes = (const BYTE*)ct"
         ";\n"
         "    assert(geometry.stateCount != 0); /* The "
         "state API accepts a successfully constructed "
         "table. */\n"
         "    statePtr->value = header.tableLog == 0 ? "
         "1 : (ptrdiff_t)geometry.stateCount;\n"
         "    statePtr->stateTable = bytes + sizeof(FSE"
         "_CTableHeader);\n"
         "    statePtr->symbolTT = bytes + geometry.tra"
         "nsformOffset;\n"
         "    statePtr->stateLog = header.tableLog;")
set(_zstd_fse_max_original
    [=[  void const* ptr = ctable;
  U16 const* u16ptr = (U16 const*)ptr;
  U32 const maxSymbolValue = MEM_read16(u16ptr + 1);
  return maxSymbolValue;]=])
set(_zstd_fse_max_checked
    [=[  FSE_CTableHeader const header = FSE_readCTableHeader(ctable);
  return header.maxSymbolValue;]=])
