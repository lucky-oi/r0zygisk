cmake_minimum_required(VERSION 3.22.1)
project("loader")

find_package(cxx REQUIRED CONFIG)

add_definitions(-DZKSU_VERSION=\"${ZKSU_VERSION}\")

aux_source_directory(common COMMON_SRC_LIST)
add_library(common STATIC ${COMMON_SRC_LIST})
target_include_directories(common PRIVATE include)
target_link_libraries(common cxx::cxx log)

aux_source_directory(injector INJECTOR_SRC_LIST)
add_library(r0z SHARED ${INJECTOR_SRC_LIST})
target_include_directories(r0z PRIVATE include)
target_link_libraries(r0z cxx::cxx log common lsplt_static)

aux_source_directory(ptracer PTRACER_SRC_LIST)
add_executable(libr0z_ptrace.so ${PTRACER_SRC_LIST})
target_include_directories(libr0z_ptrace.so PRIVATE include)
target_link_libraries(libr0z_ptrace.so cxx::cxx log common)

add_subdirectory(external)
