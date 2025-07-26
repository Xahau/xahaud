#[===================================================================[
   NIH dep: date

   the main library is header-only, thus is an INTERFACE lib in CMake.

   NOTE: this has been accepted into c++20 so can likely be replaced
   when we update to that standard
#]===================================================================]

find_package (date QUIET)
if (NOT TARGET date::date)
  # Build the timezone library
  set(BUILD_TZ_LIB ON CACHE BOOL "Build date-tz library")
  set(USE_SYSTEM_TZ_DB ON CACHE BOOL "Use system timezone database")
  set(ENABLE_DATE_TESTING OFF CACHE BOOL "Disable date tests")
  
  # Critical: Disable install/export to prevent CMake errors when used as subdirectory
  set(ENABLE_DATE_INSTALL OFF CACHE BOOL "Disable date library install")
  
  FetchContent_Declare(
    hh_date_src
    GIT_REPOSITORY https://github.com/HowardHinnant/date.git
    GIT_TAG        v3.0.4
  )
  FetchContent_MakeAvailable(hh_date_src)
  
  # The aliases should already exist in 3.0.4, but ensure they're available
  if(TARGET date-tz AND NOT TARGET date::date-tz)
    add_library(date::date-tz ALIAS date-tz)
  endif()
endif ()
