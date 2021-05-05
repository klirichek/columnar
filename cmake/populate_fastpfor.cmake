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
