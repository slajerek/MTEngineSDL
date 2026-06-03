if (NOT DEFINED OUT)
    message(FATAL_ERROR "mt_llama_cpp_archive: missing -DOUT")
endif()
if (NOT DEFINED AR)
    message(FATAL_ERROR "mt_llama_cpp_archive: missing -DAR")
endif()
if (NOT DEFINED RANLIB)
    message(FATAL_ERROR "mt_llama_cpp_archive: missing -DRANLIB")
endif()
if (NOT DEFINED LIBS)
    message(FATAL_ERROR "mt_llama_cpp_archive: missing -DLIBS")
endif()

string(REPLACE "|" ";" LIB_LIST "${LIBS}")
if (LIB_LIST STREQUAL "")
    message(FATAL_ERROR "mt_llama_cpp_archive: empty LIBS")
endif()

get_filename_component(OUT_DIR "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUT_DIR}")

set(MRI_FILE "${OUT}.mri")
file(WRITE "${MRI_FILE}" "CREATE ${OUT}\n")
foreach(lib IN LISTS LIB_LIST)
    if (NOT EXISTS "${lib}")
        message(FATAL_ERROR "mt_llama_cpp_archive: missing input archive: ${lib}")
    endif()
    file(APPEND "${MRI_FILE}" "ADDLIB ${lib}\n")
endforeach()
file(APPEND "${MRI_FILE}" "SAVE\nEND\n")

file(REMOVE "${OUT}")

execute_process(
    COMMAND "${AR}" -M
    INPUT_FILE "${MRI_FILE}"
    RESULT_VARIABLE AR_RES
)
if (NOT AR_RES EQUAL 0)
    message(FATAL_ERROR "mt_llama_cpp_archive: ar failed with code ${AR_RES}")
endif()

execute_process(
    COMMAND "${RANLIB}" "${OUT}"
    RESULT_VARIABLE RANLIB_RES
)
if (NOT RANLIB_RES EQUAL 0)
    message(FATAL_ERROR "mt_llama_cpp_archive: ranlib failed with code ${RANLIB_RES}")
endif()
