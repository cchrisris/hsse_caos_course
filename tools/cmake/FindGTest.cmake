Include(FetchContent)
Set(FETCHCONTENT_QUIET FALSE)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
  GIT_PROGRESS   TRUE
)

FetchContent_MakeAvailable(googletest)
