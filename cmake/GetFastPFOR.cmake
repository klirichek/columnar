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

include (populate_fastpfor)

get_build_folder_for ( fastpfor LIB_BUILD )
list ( APPEND CMAKE_PREFIX_PATH "${LIB_BUILD}" )

find_package ( FastPFOR QUIET)
diag ( FastPFOR_FOUND )

if (FastPFOR_FOUND)
#	include ( "CMakePrintHelpers" )
#	cmake_print_properties ( TARGETS FastPFOR::FastPFOR PROPERTIES
#			INTERFACE_COMPILE_OPTIONS
#			INTERFACE_INCLUDE_DIRECTORIES
#			IMPORTED_CONFIGURATIONS
#			IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG
#			IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE
#			IMPORTED_LOCATION_DEBUG
#			IMPORTED_LOCATION_RELEASE
#			IMPORTED_LOCATION_RELWITHDEBINFO
#			IMPORTED_LOCATION_MINSIZEREL
#			MAP_IMPORTED_CONFIG_MINSIZEREL
#			MAP_IMPORTED_CONFIG_RELWITHDEBINFO
#			MAP_IMPORTED_CONFIG_DEBUG
#			MAP_IMPORTED_CONFIG_RELEASE
#			LOCATION
#			)
	return()
endif()

set ( REMOTE_URL "https://github.com/lemire/FastPFor/archive/refs/heads/master.zip" )
set ( fastpfor_tarball "FastPFor-master.zip" )

populate ( LIB_LOCATION FastPFOR "${LIBS_BUNDLE}/${fastpfor_tarball}" "${REMOTE_URL}" )
get_srcpath ( LIB_SRC fastpfor )

set ( FASTPFOR_BUILD_DIR "${CMAKE_BINARY_DIR}/fastpfor-cache" )
configure_file ( ${CMAKE_MODULE_PATH}/fastpfor.cmake.in ${FASTPFOR_BUILD_DIR}/CMakeLists.txt )
execute_process ( COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" . WORKING_DIRECTORY ${FASTPFOR_BUILD_DIR} )
execute_process ( COMMAND ${CMAKE_COMMAND} --build . WORKING_DIRECTORY ${FASTPFOR_BUILD_DIR} )

find_package ( FastPFOR REQUIRED )
