if (__update_bundle_included)
	return ()
endif ()
set ( __update_bundle_included YES )

#diagnostic helpers
if (DEFINED ENV{DIAGNOSTIC})
	set ( DIAGNOSTIC "$ENV{DIAGNOSTIC}" )
endif ()

include ( CMakePrintHelpers )
function ( DIAG )
	if (DIAGNOSTIC)
		cmake_print_variables ( ${ARGN} )
	endif ()
endfunction ()
diag ( DIAGNOSTIC )

function ( DIAGS MSG )
	if (DIAGNOSTIC)
		message ( STATUS "${MSG}" )
	endif ()
endfunction ()

function ( infomsg MSG )
	if (NOT CMAKE_REQUIRED_QUIET)
		message ( STATUS "${MSG}" )
	endif ()
endfunction ()

# bundle - contains sources (tarballs) of 3-rd party libs. If not provided, try path 'bundle' aside sources.
# if it is provided anyway (via cmake var, ir via env var) and NOT absolute - point it into binary (build) dir.
if (DEFINED ENV{LIBS_BUNDLE})
	set ( LIBS_BUNDLE "$ENV{LIBS_BUNDLE}" )
endif ()

if (NOT LIBS_BUNDLE)
	get_filename_component ( LIBS_BUNDLE "${CMAKE_SOURCE_DIR}/../bundle" ABSOLUTE )
endif ()

if (NOT IS_ABSOLUTE ${LIBS_BUNDLE})
	get_filename_component ( LIBS_BUNDLE "${CMAKE_BINARY_DIR}/${LIBS_BUNDLE}" ABSOLUTE )
endif ()

SET ( LIBS_BUNDLE "${LIBS_BUNDLE}" CACHE PATH "Choose the path to the dir which contains all helper libs like expat, mysql, etc." FORCE )

# cacheb (means 'cache binary') - contains unpacked sources and builds of 3-rd party libs, alive between rebuilds.
# if not provided, set to folder 'cache' aside bundle. If not absolute, point it into binary (build) dir.
if (DEFINED ENV{CACHEB})
	set ( CACHEB "$ENV{CACHEB}" )
endif ()

if (NOT DEFINED CACHEB)
	get_filename_component ( CACHEB "${LIBS_BUNDLE}/../cache" ABSOLUTE )
endif ()

if (NOT IS_ABSOLUTE ${CACHEB})
	set ( CACHEB "${CMAKE_BINARY_DIR}/${CACHEB}" )
endif ()

# HAVE_BBUILD means we will build in aside folder (inside CACHEB) and then store the result for future.
if (DEFINED CACHEB)
	SET ( CACHEB "${CACHEB}" CACHE PATH "Cache dir where unpacked sources and builds found." )
	if (NOT EXISTS ${CACHEB})
		get_filename_component ( REL_BBUILD "${CACHEB}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}" )
		file ( MAKE_DIRECTORY ${REL_BBUILD} )
	endif ()
	diag ( CACHEB )
	set ( HAVE_BBUILD TRUE )
endif ()

# SUFF is line like 'darwin-x86_64' (system-arch)
# SUFFD is line like 'debug-darwin-x86_64' - that is for system with multiconfig; SUFF for release, SUFFD for debug.
SET ( SUFF "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" )
string ( TOLOWER "${SUFF}" SUFF )
if (MSVC OR XCODE OR CMAKE_BUILD_TYPE STREQUAL Debug)
	SET ( SUFF "debug-${SUFF}" )
else ()
	SET ( SUFF "${SUFF}" )
endif ()

diag ( CMAKE_BUILD_TYPE )
diag ( SUFFD )
diag ( CMAKE_SOURCE_DIR CMAKE_CURRENT_SOURCE_DIR )
diag ( CMAKE_BINARY_DIR CMAKE_CURRENT_BINARY_DIR )

# env WRITEB (as bool) means that we can store downloaded stuff to our bundle (that's to refresh the bundle)
unset ( WRITEB )
set ( WRITEB "$ENV{WRITEB}" )
if (WRITEB)
	infomsg ( "========================================================" )
	infomsg ( "WRITEB is set, will modify bundle, will collect stuff..." )
	infomsg ( "${LIBS_BUNDLE}" )
	infomsg ( "========================================================" )
	file ( MAKE_DIRECTORY ${LIBS_BUNDLE} )
else ()
	infomsg ( "WRITEB is not set, bundle will NOT be modified..." )
endif ()

diag ( WRITEB )
diag ( LIBS_BUNDLE )
diag ( CACHEB )
diag ( HAVE_BBUILD )


# get path for build folder. In case with HAVE_BBUILD it will be suffixed with platform/arch/debug flag.
function ( get_build_folder_for NAME RESULT )
	if (HAVE_BBUILD)
		set ( _RESULT "${CACHEB}/${NAME}-${SUFF}" )
		diags ( "${NAME} build will be set to ${_RESULT}" )
	else ()
		set ( _RESULT "${CMAKE_BINARY_DIR}/${NAME}" )
		diags ( "${NAME} build will be set to local ${_RESULT}" )
	endif ()
	set ( ${RESULT} "${_RESULT}" PARENT_SCOPE )
endfunction ()


# get name for source folder. It will give either local folder, or build in bundle
function ( GET_SRCPATH RESULT NAME )
	if (HAVE_BBUILD)
		set ( _RESULT "${CACHEB}/${NAME}-src" )
		diags ( "${NAME} src will be set to ${_RESULT}" )
	else ()
		set ( _RESULT "${CMAKE_BINARY_DIR}/${NAME}-src" )
		diags("${NAME} src will be set to local ${_RESULT}")
	endif ()
	set ( ${RESULT} "${_RESULT}" PARENT_SCOPE )
endfunction ()

# set PLACE to external url or to path in bundle.
# if WRITEB is active, download external url into bundle
function ( POPULATE PLACE NAME BUNDLE_URL REMOTE_URL )
	diag ( BUNDLE_URL )
	diag ( REMOTE_URL )
	if (NOT EXISTS "${BUNDLE_URL}" AND WRITEB)
		diags ( "fetch ${REMOTE_URL} into ${BUNDLE_URL}..." )
		file ( DOWNLOAD ${REMOTE_URL} ${BUNDLE_URL} SHOW_PROGRESS )
		infomsg ( "Absent ${NAME} put into ${BUNDLE_URL}" )
	endif ()

	if (EXISTS "${BUNDLE_URL}")
		set ( ${PLACE} "${BUNDLE_URL}" PARENT_SCOPE )
	else ()
		set ( ${PLACE} "${REMOTE_URL}" PARENT_SCOPE )
	endif ()

	diag ( NAME )
	diag ( BUNDLE_URL )
	diag ( REMOTE_URL )
endfunction ()

# fetches given PLACE (from bundle tarball or remote) to TARGET_SRC folder.
# archive will be fetched (from remote url or local folder) and unpacked into folder TARGET_SRC
function ( FETCH_AND_UNPACK NAME PLACE TARGET_SRC )
	diags ( "FETCH_AND_UNPACK (${NAME} ${PLACE} ${TARGET_SRC}" )
	include ( FetchContent )
	if (EXISTS ${PLACE})
		file ( SHA1 "${PLACE}" SHA1SUM )
		set ( SHA1SUM "SHA1=${SHA1SUM}" )
		FetchContent_Declare ( ${NAME} SOURCE_DIR "${TARGET_SRC}" URL "${PLACE}" URL_HASH ${SHA1SUM} )
	else ()
		FetchContent_Declare ( ${NAME} SOURCE_DIR "${TARGET_SRC}" URL "${PLACE}" )
	endif ()

	diag ( TARGET_SRC )
	diag ( PLACE )

	FetchContent_GetProperties ( ${NAME} )
	if (NOT ${NAME}_POPULATED)
		infomsg ( "Populate ${NAME} from ${PLACE}" )
		FetchContent_Populate ( ${NAME} )
	endif ()

	string ( TOUPPER "${NAME}" UNAME )
	mark_as_advanced ( FETCHCONTENT_SOURCE_DIR_${UNAME} FETCHCONTENT_UPDATES_DISCONNECTED_${UNAME} )
endfunction ()

# default if custom is not defined.
# Include BINDIR/LIB-targets.cmake. It should provide imported target - path to includes, binaries, etc.
# (SRCDIR is not necessary, added just for convenience and will be just displayed)
function ( check_lib_is_built LIB SRCDIR BINDIR )
	string ( TOLOWER "${LIB}" SLIB )

	#	diags("FIND_LIB_BUILD_DEFAULT ${LIB} ${SRCDIR} ${BINDIR}")
	if (NOT EXISTS "${BINDIR}/${SLIB}-targets.cmake")
		diags ( "not found ${SLIB}-targets.cmake" )
		return ()
	endif ()

	include ( "${BINDIR}/${SLIB}-targets.cmake" )

	if (TARGET ${LIB}::${LIB} )
		get_target_property ( INC ${LIB}::${LIB} INTERFACE_INCLUDE_DIRECTORIES )
		IF (NOT EXISTS ${INC})
			diags ( "not exists ${INC}" )
			return ()
		endif ()

		string ( TOUPPER "${CMAKE_BUILD_TYPE}" UPB )
		get_target_property ( LBB ${LIB}::${LIB} LOCATION_${UPB} )
		if (NOT EXISTS ${LBB})
			diags ( "not exists ${LBB}" )
			return ()
		endif ()
	endif()

	set ( ${LIB}_FOUND TRUE PARENT_SCOPE )
endfunction ()


# check if all found and return. That is intentially macro to write in caller's scope
macro ( RETURN_IF_FOUND LIB LEGEND )
	if (${LIB}_FOUND)
		set ( ${LIB}_INCLUDE_DIRS "${${LIB}_INCLUDE_DIRS}" PARENT_SCOPE )
		set ( ${LIB}_LIBRARIES "${${LIB}_LIBRARIES}" PARENT_SCOPE )
		set ( ${LIB}_FOUND TRUE PARENT_SCOPE )
		diags ( "${LIB} ${LEGEND} ${${LIB}_INCLUDE_DIRS} ${${LIB}_LIBRARIES}" )
		return ()
	endif ()
endmacro ()

macro ( CUSTOM_FIND_LIB LIB )
	if (COMMAND custom_find_${LIB})
		set ( __invoke_find_file "${CMAKE_CURRENT_BINARY_DIR}/__find_temp.cmake" )
		file ( WRITE "${__invoke_find_file}" "custom_find_${LIB}()" )
		include ( ${__invoke_find_file} )
	else ()
		find_package ( ${LIB} QUIET )
	endif ()
endmacro ()

# call function CHECK_FOO_SRC by name
macro ( check_lib_src LIB IS_FOUND HINT )
	if (COMMAND check_${LIB}_src)
		set ( __invoke_check_file "${CMAKE_CURRENT_BINARY_DIR}/__check_temp.cmake" )
		file ( WRITE "${__invoke_check_file}" "check_${LIB}_src(${IS_FOUND} ${HINT})" )
		include ( ${__invoke_check_file} )
	endif ()
endmacro ()

# call function PREPARE_FOO by name
function ( prepare_lib LIB LIB_SRC )
	if (COMMAND prepare_${LIB})
		set ( __invoke_prepare_file "${CMAKE_CURRENT_BINARY_DIR}/__prepare_temp.cmake" )
		file ( WRITE "${__invoke_prepare_file}" "prepare_${LIB}(${LIB_SRC})" )
		include ( ${__invoke_prepare_file} )
	endif ()
endfunction ()

# main ring that rules them all
# uses up to 4 callbacks (all are optional)
#   custom_find_LIB() - to be used instead of find_package (LIB)
#   check_LIB_src(RESULT) - to check if we have simple embedded sources, and point LIB_SRC and LIB_BUILD into it
#   prepare_LIB(SRC) - to finalize legacy unpacked sources (patch, add cmakelist, etc.)
#   find_LIB_build(SRC BIN) - to check if lib in SRC and BIN is suitable, and set LIB_INCLUDE_DIRSC,
#		LIB_LIBRARIES, and LIB_FOUND, if so.
function ( PROVIDE LIB LIB_TARBALL LIB_GITHUB )
	diags ("provide (${LIB} ${LIB_TARBALL} ${LIB_GITHUB}")
	string ( TOLOWER "${LIB}" SLIB )
	string ( TOLOWER "${LIB}" ULIB )

	diag ( LIB_TARBALL LIB_GITHUB )

	# get source tarball, if WRITEB is in action
	populate ( LIB_PLACE ${LIB} "${LIBS_BUNDLE}/${LIB_TARBALL}" "${LIB_GITHUB}" )
	get_srcpath ( LIB_SRC ${SLIB} )

	# check if user wants to try cached sources, but build them in-tree (to use custom config keys, etc)
	if (WITH_${ULIB}_FORCE_BUILD)
		# force get_buildd to find no cache for building
		unset ( __OLD_BBUILD )
		set ( __OLD_BBUILD ${HAVE_BBUILD} )
		set ( HAVE_BBUILD FALSE )
		get_build_folder_for ( ${SLIB} LIB_BUILD  )
		set ( HAVE_BBUILD ${__OLD_BBUILD} )
	else ()
		# check for cached lib
		get_build_folder_for ( ${SLIB} LIB_BUILD )
		if (HAVE_BBUILD)
			check_lib_is_built ( ${LIB} ${LIB_SRC} ${LIB_BUILD} )
			return_if_found ( ${LIB} "found in cache" )
		endif ()
	endif ()

	# check if already populated
	check_lib_src ( ${LIB} HAVE_SRC ${LIB_SRC} )
	if (HAVE_SRC)
		diags ( "${LIB} found ready source ${LIB_SRC}, will use it" )
	else ()
		diags ( "${LIB} wasn't found in-source, will fetch it" )
		# provide sources
		fetch_and_unpack ( ${LIB} ${LIB_PLACE} ${LIB_SRC} )
		prepare_lib ( ${LIB} ${LIB_SRC} )
	endif ()

	set ( ${LIB}_SRC "${LIB_SRC}" PARENT_SCOPE )
	set ( ${LIB}_BUILD "${LIB_BUILD}" PARENT_SCOPE )
	set ( ${LIB}_FROMSOURCES TRUE PARENT_SCOPE )
endfunction ()

# compare local CMakeLists.txt with target and m.b. update it, if necessary
function ( ensure_cmake_is_actual LIB_SRC library )
	set ( _src "${CMAKE_SOURCE_DIR}/${library}/CMakeLists.txt" )
	set ( _dst "${LIB_SRC}/CMakeLists.txt" )

	if (NOT EXISTS "${_dst}")
		configure_file ( "${_src}" "${_dst}" COPYONLY )
		return ()
	endif ()

	execute_process ( COMMAND ${CMAKE_COMMAND} -E compare_files "${_src}" "${_dst}" RESULT_VARIABLE _res )
	if (_res EQUAL 0)
		return ()
	elseif (_res EQUAL 1)
		infomsg ( "The ${_src} and ${_dst} are different, will refresh" )
	else ()
		infomsg ( "Error while comparing the files ${_src} and ${_dst}, assume refresh" )
	endif ()
	configure_file ( "${_src}" "${_dst}" COPYONLY )
endfunction ()
