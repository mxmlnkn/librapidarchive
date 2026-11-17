#pragma once

#include <string>
#include <vector>

#if defined( __linux__ )

#include <cerrno>
#include <cstring>
#include <iostream>

#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#endif

/**
 * The sandbox must only be applied by the real CLI binaries. In-process CLI tests and the Python
 * extension must not restrict their whole process.
 */
#if defined( __linux__ ) && !defined( WITH_PYTHON_SUPPORT ) && !defined( WITHOUT_MAIN )
    #define LIBRAPIDARCHIVE_CLI_SANDBOX 1
#else
    #define LIBRAPIDARCHIVE_CLI_SANDBOX 0
#endif


namespace rapidgzip::sandbox
{
struct Config
{
    /** Files that may still be opened for reading after the sandbox has been installed. */
    std::vector<std::string> readablePaths;
    bool verbose{ false };
    bool quiet{ false };
};


void
warn( const rapidgzip::sandbox::Config& config,
      const std::string&                message,
      const int                         errorCode )
{
    if ( !config.quiet ) {
        std::cerr << "[Warning] Could not harden the CLI process: " << message
                  << " because of: " << std::strerror( errorCode ) << " (" << errorCode << ")\n";
    }
}


void
hardenProcess( const rapidgzip::sandbox::Config& config )
{
    if ( prctl( PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0 ) != 0 ) {
        warn( config, "setting no_new_privs failed", errno );
    }

    /* Dropping the bounding set requires CAP_SETPCAP. It is best effort because the capability sets
     * below are cleared unconditionally, which is enough for a non-root process. */
    for ( int capability = 0; capability < 64; ++capability ) {
        prctl( PR_CAPBSET_DROP, capability, 0, 0, 0 );
    }

    struct __user_cap_header_struct capabilityHeader{};
    capabilityHeader.version = _LINUX_CAPABILITY_VERSION_3;
    capabilityHeader.pid = 0;
    struct __user_cap_data_struct capabilityData[2] {};
    if ( syscall( SYS_capset, &capabilityHeader, capabilityData ) != 0 ) {
        warn( config, "dropping capabilities failed", errno );
    }

    /* A core dump would persist the decompressed plaintext (and the input) to disk, where it can outlive
     * the process. Clear the soft and hard limit because the process can undo PR_SET_DUMPABLE below, but
     * cannot raise the hard limit without CAP_SYS_RESOURCE, which was dropped above. */
    const struct rlimit coreLimit{ 0, 0 };
    if ( setrlimit( RLIMIT_CORE, &coreLimit ) != 0 ) {
        warn( config, "disabling core dumps failed", errno );
    }

    /* Also keep other processes from tracing or reading this one. */
    if ( prctl( PR_SET_DUMPABLE, 0, 0, 0, 0 ) != 0 ) {
        warn( config, "clearing the dumpable flag failed", errno );
    }
}


/**
 * Drops privileges and restricts the current process to its already opened file descriptors.
 *
 * This must be called from the main thread after all input and output files have been opened and
 * before any worker threads are created because the restrictions are inherited only by threads
 * created afterwards.
 */
#if LIBRAPIDARCHIVE_CLI_SANDBOX
inline void
installIfEnabled( const Config& config )
{
    hardenProcess( config );
}
#else
inline void
installIfEnabled( const Config& )
{}
#endif
}
