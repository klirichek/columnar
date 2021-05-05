# ---------- macos ----------
# Above line is mandatory!
# rules to build tgz archive for Mac OS X

message ( STATUS "Will create TGZ with build for Mac Os X" )

# package specific
find_program ( SWVERSPROG sw_vers )
if ( SWVERSPROG )
	# use dpkg to fix the package file name
	execute_process (
			COMMAND ${SWVERSPROG} -productVersion
			OUTPUT_VARIABLE MACOSVER
			OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	mark_as_advanced ( SWVERSPROG MACOSVER )
endif ( SWVERSPROG )

if ( NOT MACOSVER )
	set ( MACOSVER "10.14" )
endif ()

set ( CPACK_GENERATOR "TGZ" )
set ( CPACK_SUFFIX "osx${MACOSVER}-x86_64" )

install ( TARGETS columnar LIBRARY DESTINATION "lib/" COMPONENT columnar )
