cmake_minimum_required(VERSION 3.20)

project(stq)

enable_language(CXX)


add_library(stq INTERFACE)
target_include_directories(stq INTERFACE include)
target_compile_features(stq INTERFACE cxx_std_17)


add_executable(tests tests/main.cpp)
target_link_libraries(tests PRIVATE stq)
if(MSVC)
else()
    target_compile_options(tests PRIVATE -Wall -Wextra -pedantic -Werror)
endif()
# target_compile_options(tests PUBLIC -fsanitize=address)
# target_link_options(tests PUBLIC -fsanitize=address)
# target_compile_options(tests PRIVATE -fsanitize=thread)
# target_link_options(tests PRIVATE -fsanitize=thread)

enable_testing()
add_test(NAME tests COMMAND tests)