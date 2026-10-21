cmake_minimum_required(VERSION 3.16)

project(exam_sdk CXX)

# sdk/ 与 tests/ 平级: 前者是交付物, 后者只在本机构建时编
add_subdirectory(sdk)
add_subdirectory(tests)
