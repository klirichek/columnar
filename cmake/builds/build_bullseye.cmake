# ---------- bullseye ----------
# Above line is mandatory!
# rules to build deb package for Debian Bullseye)

message ( STATUS "Will create DEB for Debian Bullseye" )

# we provide explicit dependencies, so shlideps is not necessary
set ( disable_shlideps ON )
set ( CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.29), libgcc1 (>= 3.0), libstdc++6 (>= 9)" )

include ( builds/CommonDeb )
