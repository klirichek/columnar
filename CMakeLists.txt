# Copyright (c) 2020-2021, Manticore Software LTD (https://manticoresearch.com)
# All rights reserved
#
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required ( VERSION 3.17 )

# 0091 is for setting msvc runtime library via property
FOREACH (policy CMP0091)
	IF (POLICY ${policy})
		CMAKE_POLICY ( SET ${policy} NEW )
	ENDIF ()
ENDFOREACH ()

project ( columnar VERSION 1.0.0 )
SET ( CMAKE_CXX_STANDARD 14 )
SET ( CMAKE_MODULE_PATH "${columnar_SOURCE_DIR}/cmake" )

# helpers vars to shorten generate lines
set ( GNUCXX "$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>" )
set ( GNUC "$<COMPILE_LANG_AND_ID:C,Clang,AppleClang,GNU>" )
set ( GNUC_CXX "$<OR:${GNUCXX},${GNUC}>" )

# main interface to collect all includes/links/dependencies
add_library ( columnar_root INTERFACE )

# options for clang/gcc c and c++
target_compile_options ( columnar_root INTERFACE $<${GNUC_CXX}:-D_FILE_OFFSET_BITS=64> )
# no rtti
target_compile_options ( columnar_root INTERFACE $<${GNUCXX}:-fno-rtti> $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/GR->)
target_include_directories ( columnar_root INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} accessor builder util )

add_library ( columnar MODULE columnar.cpp builder.cpp columnar.h builder.h )
add_library ( columnar::columnar ALIAS columnar )

target_compile_options ( columnar PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:-D_CRT_SECURE_NO_WARNINGS -wd4996> )

if (WIN32)
	set_target_properties ( columnar PROPERTIES OUTPUT_NAME lib_manticore_columnar )
else ()
	set_target_properties ( columnar PROPERTIES OUTPUT_NAME _manticore_columnar )
endif ()

include ( GetFastPFOR )
target_link_libraries ( columnar PRIVATE columnar_root )

add_subdirectory ( util )
target_link_libraries ( columnar PRIVATE util )

add_subdirectory ( builder )
target_link_libraries ( columnar PRIVATE builder )

add_subdirectory ( accessor )
target_link_libraries ( columnar PRIVATE accessor )

set_property ( TARGET columnar PROPERTY POSITION_INDEPENDENT_CODE TRUE )
set_property ( TARGET util PROPERTY POSITION_INDEPENDENT_CODE TRUE )
set_property ( TARGET builder PROPERTY POSITION_INDEPENDENT_CODE TRUE )
set_property ( TARGET accessor PROPERTY POSITION_INDEPENDENT_CODE TRUE )

set_property ( TARGET columnar PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" )
set_property ( TARGET util PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" )
set_property ( TARGET builder PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" )
set_property ( TARGET accessor PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" )

if (DEFINED DEBUG_COLUMNAR_DEST)
	add_custom_command ( TARGET columnar POST_BUILD	COMMAND ${CMAKE_COMMAND} -E rm -f "${DEBUG_COLUMNAR_DEST}/$<TARGET_FILE_NAME:columnar>" )
	add_custom_command ( TARGET columnar POST_BUILD	COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:columnar>" "${DEBUG_COLUMNAR_DEST}/."	)
	if ( MSVC )
		add_custom_command ( TARGET columnar POST_BUILD	COMMAND ${CMAKE_COMMAND} -E rm -f "${DEBUG_COLUMNAR_DEST}/$<TARGET_PDB_FILE_NAME:columnar>" )
		add_custom_command ( TARGET columnar POST_BUILD	COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_PDB_FILE:columnar>" "${DEBUG_COLUMNAR_DEST}/." )
	endif ()
endif ()

add_custom_target (
		columnarpackver
		COMMAND ${CMAKE_COMMAND}
		-D SOURCE_DIR=${columnar_SOURCE_DIR}
		-D BINARY_DIR=${columnar_BINARY_DIR}
		-D BUILD_TAG=${BUILD_TAG}
		-D DISTR_SUFFIX=${DISTR_SUFFIX}
		-D PACKAGE_NAME=${PACKAGE_NAME}
		-D CPACK_SUFFIX=${CPACK_SUFFIX}
		-D VERSION_STR=${CMAKE_PROJECT_VERSION}
		-D CMAKE_MODULE_PATH=${CMAKE_MODULE_PATH}
		-D SRC=${columnar_SOURCE_DIR}/cmake/CPackOptions.cmake.in
		-D DST=${columnar_BINARY_DIR}/config/CPackOptions.cmake
		-P ${columnar_SOURCE_DIR}/packversion.cmake
		VERBATIM
		COMMENT "Assign package version data"
		BYPRODUCTS ${columnar_BINARY_DIR}/config/CPackOptions.cmake
		SOURCES ${columnar_SOURCE_DIR}/cmake/CPackOptions.cmake.in
		DEPENDS ${columnar_SOURCE_DIR}/cmake/CPackOptions.cmake.in
)

if (PACK_COLUMNAR)
        # --------------- related with packaging -------------------

		if (NOT CMAKE_BUILD_TYPE)
			set ( CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build." FORCE)
		endif ()

        SET ( BINPREFIX "usr/" )

        set ( PACKAGE_NAME "manticore-columnar-lib" )
        set ( COMPANY_NAME "Manticore Software LTD" )
        set ( BUILD_TAG "" CACHE STRING "The tag which will be added to build" )

        # simpler packages: provide -DPACK=1, and DISTR_BUILD will be set from env $DISTR, easier in dockers
        if (PACK)
                set(DISTR_BUILD "$ENV{DISTR}")
        endif ()


        # Make release build for the pointed distr
        # That will override defaults and give possibility to build
        # the distribution with minimal command line
        if ( DISTR_BUILD )
               # set default options to be included into build
               set ( DISTR "${DISTR_BUILD}" CACHE STRING "Choose the distr." )
               if ( NOT "${DISTR_BUILD}" STREQUAL "macosbrew" )
                      SET ( CMAKE_INSTALL_PREFIX "/usr" CACHE FILEPATH "Forced install to /usr" FORCE )
               endif ()
               message ( STATUS "DISTR_BUILD applied. Package will be set to ${DISTR_BUILD}" )
        endif ()

        include ( SetBuildType )

        configure_file ( "LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/LICENSE" COPYONLY )

        set ( CPACK_SOURCE_IGNORE_FILES "/\\\\.idea/" "/\\\\.git/"
                "/\\\\.svn/" "/autom4te\\\\.cache/" "/bin/" "/junk/" "/win/" "/gmock/" "/gtest/" "/libtest"   "/pkgconfig/"
                ".swp$" ".orig$" ".log$" ".deps/" )
        set ( CPACK_SOURCE_GENERATOR TGZ )
        set ( CPACK_PACKAGE_NAME "${PACKAGE_NAME}" )
        set ( CPACK_PACKAGE_CONTACT "Manticore Team <build@manticoresearch.com>" )
        set ( CMAKE_PROJECT_HOMEPAGE_URL "https://github.com/manticoresoftware/columnar/" )
		set ( CPACK_PACKAGE_URL "https://github.com/manticoresoftware/columnar/" )
		set ( CPACK_PACKAGE_GROUP "Applications/Internet" )
		set ( CPACK_PACKAGE_VENDOR "${COMPANY_NAME}" )
        set ( CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE" )
        set ( CPACK_PACKAGE_DESCRIPTION_SUMMARY "Manticore Columnar Library is a column-oriented storage library, aiming to provide decent performance with low memory footprint at big data volume")
        set ( CPACK_PACKAGE_DESCRIPTION "Manticore Columnar Library is a column-oriented storage library, aiming to provide decent performance with low memory footprint at big data volume. When used in combination with Manticore Search can be beneficial for faster / lower resource consumption log/metrics analytics and running log / metric analytics in docker / kubernetes")
        set ( CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}" ) # the description will default to the default one (This is an installer created using CPack..." otherwise, i.e. it doesn't take CPACK_PACKAGE_DESCRIPTION as a default 
        set ( CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/config/CPackOptions.cmake" )

        include ( CPack )
        cpack_add_component( columnar )
		cpack_add_component ( devel )

        if ( WIN32 )
                set ( CPACK_SUFFIX "x64" )
        endif ()

		add_dependencies ( columnar columnarpackver )
endif()
