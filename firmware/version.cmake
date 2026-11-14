# Firmware version and commit, stamped into the build (printed on the serial console at boot and shown on the
# status page).
#   A80_VERSION  release version YYYY.MM.NN from the environment; development builds use the placeholder 9999.99.99
#   A80_COMMIT   short commit hash from the environment (CI sets it), otherwise from git, otherwise "unknown"
if(DEFINED ENV{A80_VERSION} AND NOT "$ENV{A80_VERSION}" STREQUAL "")
    set(A80_VERSION "$ENV{A80_VERSION}")
else()
    set(A80_VERSION "9999.99.99")
endif()

if(DEFINED ENV{A80_COMMIT} AND NOT "$ENV{A80_COMMIT}" STREQUAL "")
    set(A80_COMMIT "$ENV{A80_COMMIT}")
else()
    execute_process(COMMAND git rev-parse HEAD
                    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                    OUTPUT_VARIABLE A80_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE a80_git_rc ERROR_QUIET)
    if(NOT a80_git_rc EQUAL 0 OR "${A80_COMMIT}" STREQUAL "")
        set(A80_COMMIT "unknown")
    endif()
endif()
string(LENGTH "${A80_COMMIT}" a80_commit_len)
if(a80_commit_len GREATER 7)
    string(SUBSTRING "${A80_COMMIT}" 0 7 A80_COMMIT)
endif()
