cmake_minimum_required(VERSION 3.20)
project(hidl-gen-standalone CXX C)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wno-error -Wno-deprecated-declarations "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat.h")
set(WNV "$ENV{HOME}/repos/waydroid-nv" CACHE PATH "prereq tree root (override with -DWNV=)")
set(CORE ${WNV}/aosp_system_core)


set(HIDL ${WNV}/aosp_hidl)
set(LIBBASE ${WNV}/aosp_libbase)
set(LIBLOG ${WNV}/android_system_logging/liblog)
set(FMTLIB ${WNV}/aosp_fmtlib)

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

# ---------------- liblog (host, minimal) ----------------
add_library(log STATIC
  ${LIBLOG}/log_event_list.cpp
  ${LIBLOG}/log_event_write.cpp
  ${LIBLOG}/logger_name.cpp
  ${LIBLOG}/logger_write.cpp
  ${LIBLOG}/logprint.cpp
  ${LIBLOG}/properties.cpp
  ${LIBLOG}/log_time.cpp
)
target_include_directories(log PUBLIC ${LIBLOG}/include ${CORE}/libcutils/include ${LIBBASE}/include ${FMTLIB}/include)
target_compile_definitions(log PRIVATE -DLIBLOG_LOG_TAG=1006 -D_XOPEN_SOURCE=700 -DFAKE_LOG_DEVICE=1)

# ---------------- libbase (host) ----------------
add_library(base STATIC
  ${LIBBASE}/abi_compatibility.cpp
  ${LIBBASE}/chrono_utils.cpp
  ${LIBBASE}/cmsg.cpp
  ${LIBBASE}/file.cpp
  ${LIBBASE}/hex.cpp
  ${LIBBASE}/logging.cpp
  ${LIBBASE}/mapped_file.cpp
  ${LIBBASE}/parsebool.cpp
  ${LIBBASE}/parsenetaddress.cpp
  ${LIBBASE}/posix_strerror_r.cpp
  ${LIBBASE}/process.cpp
  ${LIBBASE}/properties.cpp
  ${LIBBASE}/stringprintf.cpp
  ${LIBBASE}/strings.cpp
  ${LIBBASE}/threads.cpp
  ${LIBBASE}/test_utils.cpp
  ${FMTLIB}/src/format.cc
)
target_include_directories(base PUBLIC ${LIBBASE}/include ${FMTLIB}/include)
target_link_libraries(base PUBLIC log)

# ---------------- hidl-gen libs ----------------
add_library(hidl-gen-host-utils STATIC
  ${HIDL}/host_utils/Formatter.cpp
  ${HIDL}/host_utils/StringHelper.cpp
)
target_include_directories(hidl-gen-host-utils PUBLIC ${HIDL}/host_utils/include)
target_include_directories(hidl-gen-host-utils PRIVATE ${HIDL}/host_utils/include/hidl-util)
target_link_libraries(hidl-gen-host-utils PUBLIC base)

add_library(hidl-gen-utils STATIC
  ${HIDL}/utils/FQName.cpp
  ${HIDL}/utils/FqInstance.cpp
)
target_include_directories(hidl-gen-utils PUBLIC ${HIDL}/utils/include)
target_include_directories(hidl-gen-utils PRIVATE ${HIDL}/utils/include/hidl-util)
target_link_libraries(hidl-gen-utils PUBLIC base hidl-gen-host-utils)

add_library(hidl-gen-hash STATIC
  ${HIDL}/hashing/Hash.cpp
)
target_include_directories(hidl-gen-hash PUBLIC ${HIDL}/hashing/include)
target_link_libraries(hidl-gen-hash PUBLIC base OpenSSL::Crypto)

add_library(hidl-gen-core STATIC
  ${HIDL}/Annotation.cpp
  ${HIDL}/ArrayType.cpp
  ${HIDL}/CompoundType.cpp
  ${HIDL}/ConstantExpression.cpp
  ${HIDL}/DeathRecipientType.cpp
  ${HIDL}/DocComment.cpp
  ${HIDL}/EnumType.cpp
  ${HIDL}/FmqType.cpp
  ${HIDL}/HandleType.cpp
  ${HIDL}/HidlTypeAssertion.cpp
  ${HIDL}/Interface.cpp
  ${HIDL}/Location.cpp
  ${HIDL}/MemoryType.cpp
  ${HIDL}/Method.cpp
  ${HIDL}/NamedType.cpp
  ${HIDL}/PointerType.cpp
  ${HIDL}/ScalarType.cpp
  ${HIDL}/Scope.cpp
  ${HIDL}/StringType.cpp
  ${HIDL}/Type.cpp
  ${HIDL}/TypeDef.cpp
  ${HIDL}/VectorType.cpp
)
target_include_directories(hidl-gen-core PUBLIC ${HIDL} ${JSONCPP_INCLUDE_DIRS} ${CORE}/libutils/include ${CORE}/libcutils/include ${WNV}/aosp_libhwbinder/include)
target_link_libraries(hidl-gen-core PUBLIC base hidl-gen-utils hidl-gen-host-utils hidl-gen-hash ${JSONCPP_LIBRARIES})

BISON_TARGET(hidl_y ${HIDL}/hidl-gen_y.yy ${CMAKE_CURRENT_BINARY_DIR}/hidl-gen_y.cpp
             DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/hidl-gen_y.h)
FLEX_TARGET(hidl_l ${HIDL}/hidl-gen_l.ll ${CMAKE_CURRENT_BINARY_DIR}/hidl-gen_l.cpp)
ADD_FLEX_BISON_DEPENDENCY(hidl_l hidl_y)

add_library(hidl-gen-ast STATIC
  ${HIDL}/AST.cpp
  ${HIDL}/Coordinator.cpp
  ${HIDL}/generateCpp.cpp
  
  ${HIDL}/generateCppImpl.cpp
  ${HIDL}/generateDependencies.cpp
  ${HIDL}/generateFormattedHidl.cpp
  ${HIDL}/generateInheritanceHierarchy.cpp
  ${HIDL}/generateJava.cpp
  ${HIDL}/generateJavaImpl.cpp
  ${HIDL}/generateVts.cpp
  ${BISON_hidl_y_OUTPUTS}
  ${FLEX_hidl_l_OUTPUTS}
)
target_include_directories(hidl-gen-ast PUBLIC ${HIDL} ${CMAKE_CURRENT_BINARY_DIR} ${JSONCPP_INCLUDE_DIRS})
target_link_libraries(hidl-gen-ast PUBLIC hidl-gen-core)

add_executable(hidl-gen ${HIDL}/main.cpp)
target_link_libraries(hidl-gen PRIVATE hidl-gen-ast hidl-gen-core hidl-gen-hash hidl-gen-utils hidl-gen-host-utils base log)
set_source_files_properties(${LIBBASE}/posix_strerror_r.cpp PROPERTIES COMPILE_OPTIONS "-U_GNU_SOURCE")
