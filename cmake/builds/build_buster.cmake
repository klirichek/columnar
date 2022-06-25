# ---------- buster ----------
# Above line is mandatory!
# rules to build deb package for Debian Buster)

message ( STATUS "Will create DEB for Debian Buster" )

# we provide explicit dependencies, so shlideps is not necessary
set ( disable_shlideps ON )
set ( CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.14), libgcc1 (>= 3.0), libstdc++6 (>= 5.2)" )

include ( builds/CommonDeb )
