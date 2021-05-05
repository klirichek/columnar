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
set ( REMOTE_URL "https://github.com/lemire/FastPFor/archive/refs/heads/master.zip" )

# cb to realize if we have in-source unpacked FastPFOR
function ( CHECK_FASTPFOR_SRC RESULT HINT )
	if (EXISTS "${HINT}/LICENSE")
		ensure_cmake_is_actual ( "${HINT}" libfastpfor )
		set ( ${RESULT} TRUE PARENT_SCOPE )
	endif ()
endfunction ()

# cb to finalize FastPFOR unpacked sources (add custom cmake)
function ( PREPARE_FASTPFOR LIB_SRC )
	diags ( "PREPARE_FASTPFOR invoked" )
	ensure_cmake_is_actual ( "${LIB_SRC}" libfastpfor )
endfunction ()

include (populate_fastpfor)
PROVIDE( FastPFOR FastPFor-master.zip ${REMOTE_URL} )

if (FastPFOR_FROMSOURCES)
	add_subdirectory ( ${FastPFOR_SRC} ${FastPFOR_BUILD} EXCLUDE_FROM_ALL )
	set ( FastPFOR_FOUND TRUE )
endif ()

diag ( FastPFOR_FROMSOURCES FastPFOR_FOUND FastPFOR_SRC FastPFOR_BUILD FastPFOR_FROMSOURCES)

list ( APPEND EXTRA_LIBRARIES FastPFOR )