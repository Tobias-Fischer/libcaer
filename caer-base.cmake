FUNCTION(CAER_SETUP NEED_CPP17)

# Compiler cache support
FIND_PROGRAM(CCACHE_FOUND ccache)
IF (CCACHE_FOUND)
	SET_PROPERTY(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
	SET_PROPERTY(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
ENDIF()

# Set compiler info
SET(CC_CLANG FALSE)
SET(CC_GCC FALSE)
SET(CC_ICC FALSE)
SET(CC_MSVC FALSE)

IF ("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
	SET(CC_CLANG TRUE)
ELSEIF ("${CMAKE_C_COMPILER_ID}" STREQUAL "AppleClang")
	SET(CC_CLANG TRUE)
ELSEIF ("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
	SET(CC_GCC TRUE)
ELSEIF ("${CMAKE_C_COMPILER_ID}" STREQUAL "Intel")
	SET(CC_ICC TRUE)
ELSEIF ("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
	SET(CC_MSVC TRUE)
ENDIF()

SET(CC_CLANG ${CC_CLANG} PARENT_SCOPE)
SET(CC_GCC ${CC_GCC} PARENT_SCOPE)
SET(CC_ICC ${CC_ICC} PARENT_SCOPE)
SET(CC_MSVC ${CC_MSVC} PARENT_SCOPE)

# Set operating system info
SET(OS_UNIX FALSE)
SET(OS_LINUX FALSE)
SET(OS_MACOSX FALSE)
SET(OS_WINDOWS FALSE)

IF (UNIX)
	SET(OS_UNIX TRUE)
	ADD_DEFINITIONS(-DOS_UNIX=1)
ENDIF()

IF (UNIX AND "${CMAKE_SYSTEM_NAME}" MATCHES "Linux")
	SET(OS_LINUX TRUE)
	ADD_DEFINITIONS(-DOS_LINUX=1)
ENDIF()

IF (UNIX AND APPLE AND "${CMAKE_SYSTEM_NAME}" MATCHES "Darwin")
	SET(OS_MACOSX TRUE)
	ADD_DEFINITIONS(-DOS_MACOSX=1)
ENDIF()

IF (WIN32 AND "${CMAKE_SYSTEM_NAME}" MATCHES "Windows")
	SET(OS_WINDOWS TRUE)
	ADD_DEFINITIONS(-DOS_WINDOWS=1)
ENDIF()

SET(OS_UNIX ${OS_UNIX} PARENT_SCOPE)
SET(OS_LINUX ${OS_LINUX} PARENT_SCOPE)
SET(OS_MACOSX ${OS_MACOSX} PARENT_SCOPE)
SET(OS_WINDOWS ${OS_WINDOWS} PARENT_SCOPE)

IF (NEED_CPP17)
	# Check GCC compiler version, 7.0 is needed at least for C++17 support.
	IF (CC_GCC)
		IF (${CMAKE_C_COMPILER_VERSION} VERSION_LESS "7.0.0")
			MESSAGE(FATAL_ERROR "GCC version found is ${CMAKE_C_COMPILER_VERSION}. Required >= 7.0.0.")
		ENDIF()
	ENDIF()

	# Check Clang compiler version, 5.0 is needed at least for C++17 support.
	IF (CC_CLANG)
		IF (${CMAKE_C_COMPILER_VERSION} VERSION_LESS "5.0.0")
			MESSAGE(FATAL_ERROR "Clang version found is ${CMAKE_C_COMPILER_VERSION}. Required >= 5.0.0.")
		ENDIF()
	ENDIF()
ELSE()
	# Check GCC compiler version, 4.9 is needed at least for atomics support.
	IF (CC_GCC)
		IF (${CMAKE_C_COMPILER_VERSION} VERSION_LESS "4.9.0")
			MESSAGE(FATAL_ERROR "GCC version found is ${CMAKE_C_COMPILER_VERSION}. Required >= 4.9.0.")
		ENDIF()
	ENDIF()

	# Check Clang compiler version, 3.6 is needed at least for atomics support.
	IF (CC_CLANG)
		IF (${CMAKE_C_COMPILER_VERSION} VERSION_LESS "3.6.0")
			MESSAGE(FATAL_ERROR "Clang version found is ${CMAKE_C_COMPILER_VERSION}. Required >= 3.6.0.")
		ENDIF()
	ENDIF()
ENDIF()

# Test if we are on a big-endian architecture
INCLUDE(TestBigEndian)
TEST_BIG_ENDIAN(SYSTEM_BIGENDIAN)

# Check size of various types
INCLUDE(CheckTypeSize)
CHECK_TYPE_SIZE("size_t" SIZEOF_SIZE_T)
CHECK_TYPE_SIZE("void *" SIZEOF_VOID_PTR)

IF (NOT "${SIZEOF_VOID_PTR}" STREQUAL "${SIZEOF_SIZE_T}")
	MESSAGE(FATAL_ERROR "Size of void * and size_t must be the same!")
ENDIF()

# Check threads support (almost nobody implements C11 threads yet! C++11 is fine)
SET(CMAKE_THREAD_PREFER_PTHREAD TRUE)
FIND_PACKAGE(Threads REQUIRED)
SET(HAVE_PTHREADS FALSE)
SET(HAVE_WIN32_THREADS FALSE)

IF (DEFINED "CMAKE_USE_PTHREADS_INIT")
	IF (${CMAKE_USE_PTHREADS_INIT})
		SET(HAVE_PTHREADS TRUE)
		ADD_DEFINITIONS(-DHAVE_PTHREADS=1)
	ENDIF()
ENDIF()

IF (DEFINED "CMAKE_USE_WIN32_THREADS_INIT")
	IF (${CMAKE_USE_WIN32_THREADS_INIT})
		SET(HAVE_WIN32_THREADS TRUE)
		ADD_DEFINITIONS(-DHAVE_WIN32_THREADS=1)
	ENDIF()
ENDIF()

SET(HAVE_PTHREADS ${HAVE_PTHREADS} PARENT_SCOPE)
SET(HAVE_WIN32_THREADS ${HAVE_WIN32_THREADS} PARENT_SCOPE)
SET(SYSTEM_THREAD_LIBS ${CMAKE_THREAD_LIBS_INIT} PARENT_SCOPE)

# Add system defines for header features
IF (OS_UNIX AND HAVE_PTHREADS)
	# POSIX system (Unix, Linux, MacOS X)
	ADD_DEFINITIONS(-D_XOPEN_SOURCE=700)
	ADD_DEFINITIONS(-D_DEFAULT_SOURCE=1)

	IF (OS_MACOSX)
		ADD_DEFINITIONS(-D_DARWIN_C_SOURCE=1)
	ENDIF()

	# Support for large files (>2GB) on 32-bit systems
	ADD_DEFINITIONS(-D_FILE_OFFSET_BITS=64)
ENDIF()

IF (OS_WINDOWS AND (CC_GCC OR CC_CLANG))
	ADD_DEFINITIONS(-D__USE_MINGW_ANSI_STDIO=1)
ENDIF()

IF (NOT USER_LOCAL_PREFIX)
	SET(USER_LOCAL_PREFIX ${CMAKE_INSTALL_PREFIX})
	SET(USER_LOCAL_PREFIX ${USER_LOCAL_PREFIX} PARENT_SCOPE)
ENDIF()

# Set C/C++ compiler options
IF (CC_GCC OR CC_CLANG)
	# C11 standard needed (atomics, threads)
	SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c11")
	IF (NEED_CPP17)
		SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
	ELSE()
		SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
	ENDIF()

	# Enable all warnings for GCC / Clang
	SET(WARN_COMMON_FLAGS "-pedantic -Wall -Wextra")
	SET(WARN_C_FLAGS "")
	SET(WARN_CXX_FLAGS "")

	IF (CC_GCC)
		# Enable all useful warnings in GCC one-by-one.
		SET(WARN_COMMON_FLAGS "${WARN_COMMON_FLAGS} -Wunused -Wundef -Wformat=2 -Wuninitialized -Winit-self")
		SET(WARN_COMMON_FLAGS "${WARN_COMMON_FLAGS} -Wpointer-arith -Wcast-qual -Wcast-align -Wwrite-strings")
		SET(WARN_COMMON_FLAGS "${WARN_COMMON_FLAGS} -Wredundant-decls -Wmissing-declarations -Wdouble-promotion")
		SET(WARN_COMMON_FLAGS "${WARN_COMMON_FLAGS} -Wshadow -Wfloat-equal -Wconversion -Wstrict-overflow=5")
		SET(WARN_COMMON_FLAGS "${WARN_COMMON_FLAGS} -Wduplicated-cond -Wduplicated-branches -Wlogical-op")

		SET(WARN_C_FLAGS "${WARN_C_FLAGS} -Wstrict-prototypes -Wmissing-prototypes -Wnested-externs")
		SET(WARN_C_FLAGS "${WARN_C_FLAGS} -Wbad-function-cast -Wjump-misses-init")

		SET(WARN_CXX_FLAGS "${WARN_CXX_FLAGS} -Wold-style-cast -Wuseless-cast -Wcatch-value=3 -Wextra-semi")
		SET(WARN_CXX_FLAGS "${WARN_CXX_FLAGS} -Wzero-as-null-pointer-constant -Wno-invalid-offsetof")
	ENDIF()

	IF (CC_CLANG)
		# Enable all warnings in Clang, then turn off useless ones.
		SET(WARN_COMMON_FLAGS "${WARN_COMMON_FLAGS} -Weverything -Wno-packed -Wno-padded -Wno-unreachable-code-break")
		SET(WARN_COMMON_FLAGS "${WARN_COMMON_FLAGS} -Wno-disabled-macro-expansion -Wno-reserved-id-macro -Wno-vla")
		SET(WARN_COMMON_FLAGS "${WARN_COMMON_FLAGS} -Wno-parentheses-equality -Wno-covered-switch-default")
		SET(WARN_COMMON_FLAGS "${WARN_COMMON_FLAGS} -Wno-used-but-marked-unused -Wno-cast-align")

		SET(WARN_CXX_FLAGS "${WARN_CXX_FLAGS} -Wno-c++98-compat -Wno-c++98-compat-pedantic")
		SET(WARN_CXX_FLAGS "${WARN_CXX_FLAGS} -Wno-global-constructors -Wno-exit-time-destructors")
	ENDIF()

	# Apply all flags.
	SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${WARN_COMMON_FLAGS} ${WARN_C_FLAGS}")
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${WARN_COMMON_FLAGS} ${WARN_CXX_FLAGS}")

	SET(CMAKE_C_FLAGS ${CMAKE_C_FLAGS} PARENT_SCOPE)
	SET(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} PARENT_SCOPE)
ENDIF()

# RPATH settings
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE PARENT_SCOPE)
IF (OS_MACOSX)
	SET(CMAKE_MACOSX_RPATH TRUE PARENT_SCOPE)
ENDIF()

# Linker settings
IF (OS_UNIX AND NOT OS_MACOSX)
	# Add --as-needed to linker flags for executables.
	SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--as-needed" PARENT_SCOPE)

	# Add --as-needed to linker flags for shared libraries.
	SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--as-needed" PARENT_SCOPE)
ENDIF()

# Print info summary for debug purposes
MESSAGE(STATUS "Project name is: ${PROJECT_NAME}")
MESSAGE(STATUS "Project version is: ${PROJECT_VERSION}")
MESSAGE(STATUS "Build type is: ${CMAKE_BUILD_TYPE}")
MESSAGE(STATUS "Compiler is Clang: ${CC_CLANG}")
MESSAGE(STATUS "Compiler is GCC: ${CC_GCC}")
MESSAGE(STATUS "Compiler is IntelCC: ${CC_ICC}")
MESSAGE(STATUS "Compiler is MS VisualC: ${CC_MSVC}")
MESSAGE(STATUS "OS is Unix: ${OS_UNIX}")
MESSAGE(STATUS "OS is Linux: ${OS_LINUX}")
MESSAGE(STATUS "OS is MacOS X: ${OS_MACOSX}")
MESSAGE(STATUS "OS is Windows: ${OS_WINDOWS}")
MESSAGE(STATUS "System is big-endian: ${SYSTEM_BIGENDIAN}")
MESSAGE(STATUS "Thread support is PThreads: ${HAVE_PTHREADS}")
MESSAGE(STATUS "Thread support is Win32 Threads: ${HAVE_WIN32_THREADS}")
MESSAGE(STATUS "Thread support libraries: ${CMAKE_THREAD_LIBS_INIT}")
MESSAGE(STATUS "C flags are: ${CMAKE_C_FLAGS}")
MESSAGE(STATUS "CXX flags are: ${CMAKE_CXX_FLAGS}")
MESSAGE(STATUS "Final install bindir is: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
MESSAGE(STATUS "Final install libdir is: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
MESSAGE(STATUS "Final install includedir is: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}")
MESSAGE(STATUS "Final local prefix is: ${USER_LOCAL_PREFIX}")

ENDFUNCTION()
