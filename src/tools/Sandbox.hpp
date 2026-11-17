#pragma once

#include <string>
#include <vector>

#if defined( __linux__ )

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined( __has_include )
    #if __has_include(<linux/landlock.h>)
        #include <linux/landlock.h>
        #define LIBRAPIDARCHIVE_HAS_LANDLOCK_HEADERS 1
    #endif
#endif

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


#if defined( __linux__ )

inline void
warn( const rapidgzip::sandbox::Config& config,
      const std::string&                message,
      const int                         errorCode )
{
    if ( !config.quiet ) {
        std::cerr << "[Warning] CLI sandbox: " << message
                  << " because of: " << std::strerror( errorCode ) << " (" << errorCode << ")\n";
    }
}


inline void
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


[[nodiscard]] constexpr std::uint32_t
nativeAuditArchitecture()
{
#if defined( __x86_64__ )
    return AUDIT_ARCH_X86_64;
#elif defined( __i386__ )
    return AUDIT_ARCH_I386;
#elif defined( __aarch64__ )
    return AUDIT_ARCH_AARCH64;
#elif defined( __arm__ )
    return AUDIT_ARCH_ARM;
#elif defined( __riscv ) && ( __riscv_xlen == 64 )
    return AUDIT_ARCH_RISCV64;
#elif defined( __powerpc64__ ) && ( __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ )
    return AUDIT_ARCH_PPC64LE;
#elif defined( __powerpc64__ )
    return AUDIT_ARCH_PPC64;
#elif defined( __s390x__ )
    return AUDIT_ARCH_S390X;
#else
    return 0;
#endif
}


inline void
installSeccompFilter( const rapidgzip::sandbox::Config& config )
{
    constexpr auto nativeArchitecture = nativeAuditArchitecture();
    if ( nativeArchitecture == 0 ) {
        warn( config, "seccomp is not supported for this architecture", ENOTSUP );
        return;
    }

    std::vector<sock_filter> filter;
    filter.reserve( 128 );

    /* Reject syscalls from another ABI. On x86_64 this also catches the i386 int 0x80 ABI. */
    filter.push_back( BPF_STMT( BPF_LD | BPF_W | BPF_ABS, offsetof( struct seccomp_data, arch ) ) );
    filter.push_back( BPF_JUMP( BPF_JMP | BPF_JEQ | BPF_K, nativeArchitecture, 1, 0 ) );
    filter.push_back( BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS ) );
    filter.push_back( BPF_STMT( BPF_LD | BPF_W | BPF_ABS, offsetof( struct seccomp_data, nr ) ) );

#if defined( __x86_64__ )
    /* The x32 ABI uses the x86_64 audit architecture but sets this bit in the syscall number. */
    filter.push_back( BPF_JUMP( BPF_JMP | BPF_JGE | BPF_K, 0x40000000U, 0, 1 ) );
    filter.push_back( BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM ) );
#endif

    const auto deny =
        [&filter] ( const int syscallNumber )
        {
            filter.push_back( BPF_JUMP( BPF_JMP | BPF_JEQ | BPF_K,
                                        static_cast<std::uint32_t>( syscallNumber ), 0, 1 ) );
            filter.push_back( BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM ) );
        };

    /* The decoders never need to create sockets or execute other programs. */
#ifdef __NR_socket
    deny( __NR_socket );
#endif
#ifdef __NR_socketpair
    deny( __NR_socketpair );
#endif
#ifdef __NR_socketcall
    /* i386 multiplexes all socket calls through this single syscall. */
    deny( __NR_socketcall );
#endif
#ifdef __NR_connect
    deny( __NR_connect );
#endif
#ifdef __NR_bind
    deny( __NR_bind );
#endif
#ifdef __NR_listen
    deny( __NR_listen );
#endif
#ifdef __NR_accept
    deny( __NR_accept );
#endif
#ifdef __NR_accept4
    deny( __NR_accept4 );
#endif
#ifdef __NR_sendto
    deny( __NR_sendto );
#endif
#ifdef __NR_sendmsg
    deny( __NR_sendmsg );
#endif
#ifdef __NR_sendmmsg
    deny( __NR_sendmmsg );
#endif
#ifdef __NR_recvfrom
    deny( __NR_recvfrom );
#endif
#ifdef __NR_recvmsg
    deny( __NR_recvmsg );
#endif
#ifdef __NR_recvmmsg
    deny( __NR_recvmmsg );
#endif
#ifdef __NR_shutdown
    deny( __NR_shutdown );
#endif
#ifdef __NR_execve
    deny( __NR_execve );
#endif
#ifdef __NR_execveat
    deny( __NR_execveat );
#endif
#ifdef __NR_ptrace
    deny( __NR_ptrace );
#endif
#ifdef __NR_process_vm_readv
    deny( __NR_process_vm_readv );
#endif
#ifdef __NR_process_vm_writev
    deny( __NR_process_vm_writev );
#endif
#ifdef __NR_pidfd_getfd
    deny( __NR_pidfd_getfd );
#endif

    /* Mount, namespace, and module manipulation. */
#ifdef __NR_mount
    deny( __NR_mount );
#endif
#ifdef __NR_umount2
    deny( __NR_umount2 );
#endif
#ifdef __NR_pivot_root
    deny( __NR_pivot_root );
#endif
#ifdef __NR_chroot
    deny( __NR_chroot );
#endif
#ifdef __NR_unshare
    deny( __NR_unshare );
#endif
#ifdef __NR_setns
    deny( __NR_setns );
#endif
#ifdef __NR_move_mount
    deny( __NR_move_mount );
#endif
#ifdef __NR_open_tree
    deny( __NR_open_tree );
#endif
#ifdef __NR_fsopen
    deny( __NR_fsopen );
#endif
#ifdef __NR_fsconfig
    deny( __NR_fsconfig );
#endif
#ifdef __NR_fsmount
    deny( __NR_fsmount );
#endif
#ifdef __NR_fspick
    deny( __NR_fspick );
#endif
#ifdef __NR_mount_setattr
    deny( __NR_mount_setattr );
#endif
#ifdef __NR_init_module
    deny( __NR_init_module );
#endif
#ifdef __NR_finit_module
    deny( __NR_finit_module );
#endif
#ifdef __NR_delete_module
    deny( __NR_delete_module );
#endif
#ifdef __NR_kexec_load
    deny( __NR_kexec_load );
#endif
#ifdef __NR_kexec_file_load
    deny( __NR_kexec_file_load );
#endif
#ifdef __NR_reboot
    deny( __NR_reboot );
#endif
#ifdef __NR_swapon
    deny( __NR_swapon );
#endif
#ifdef __NR_swapoff
    deny( __NR_swapoff );
#endif
#ifdef __NR_sethostname
    deny( __NR_sethostname );
#endif
#ifdef __NR_setdomainname
    deny( __NR_setdomainname );
#endif
#ifdef __NR_acct
    deny( __NR_acct );
#endif
#ifdef __NR_quotactl
    deny( __NR_quotactl );
#endif

    /* Kernel attack surface that a decoder has no use for. */
#ifdef __NR_bpf
    deny( __NR_bpf );
#endif
#ifdef __NR_perf_event_open
    deny( __NR_perf_event_open );
#endif
#ifdef __NR_userfaultfd
    deny( __NR_userfaultfd );
#endif
    /* seccomp does not see the file and network operations performed by io_uring workers, so allowing
     * it would create a path around this filter. It is also a large attack surface for a decoder. */
#ifdef __NR_io_uring_setup
    deny( __NR_io_uring_setup );
#endif
#ifdef __NR_io_uring_enter
    deny( __NR_io_uring_enter );
#endif
#ifdef __NR_io_uring_register
    deny( __NR_io_uring_register );
#endif
#ifdef __NR_keyctl
    deny( __NR_keyctl );
#endif
#ifdef __NR_add_key
    deny( __NR_add_key );
#endif
#ifdef __NR_request_key
    deny( __NR_request_key );
#endif
#ifdef __NR_open_by_handle_at
    deny( __NR_open_by_handle_at );
#endif

    filter.push_back( BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ALLOW ) );

    struct sock_fprog program{};
    program.len = static_cast<unsigned short int>( filter.size() );
    program.filter = filter.data();
    if ( prctl( PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program ) != 0 ) {
        warn( config, "installing the seccomp filter failed", errno );
    }
}


#if defined( LIBRAPIDARCHIVE_HAS_LANDLOCK_HEADERS ) \
    && defined( __NR_landlock_create_ruleset ) \
    && defined( __NR_landlock_add_rule ) \
    && defined( __NR_landlock_restrict_self )
[[nodiscard]] inline int
landlockAbiVersion()
{
    return static_cast<int>( syscall( __NR_landlock_create_ruleset, nullptr, 0u, LANDLOCK_CREATE_RULESET_VERSION ) );
}


[[nodiscard]] inline bool
landlockIsAvailable()
{
    return landlockAbiVersion() >= 1;
}


inline void
installLandlockSandbox( const rapidgzip::sandbox::Config& config )
{
    const int abiVersion = landlockAbiVersion();
    if ( abiVersion < 1 ) {
        if ( config.verbose && !config.quiet ) {
            std::cerr << "[Info] Landlock is not available. Continuing without it.\n";
        }
        return;
    }

    std::uint64_t handledAccess =
        LANDLOCK_ACCESS_FS_EXECUTE
        | LANDLOCK_ACCESS_FS_WRITE_FILE
        | LANDLOCK_ACCESS_FS_READ_FILE
        | LANDLOCK_ACCESS_FS_READ_DIR
        | LANDLOCK_ACCESS_FS_REMOVE_DIR
        | LANDLOCK_ACCESS_FS_REMOVE_FILE
        | LANDLOCK_ACCESS_FS_MAKE_CHAR
        | LANDLOCK_ACCESS_FS_MAKE_DIR
        | LANDLOCK_ACCESS_FS_MAKE_REG
        | LANDLOCK_ACCESS_FS_MAKE_SOCK
        | LANDLOCK_ACCESS_FS_MAKE_FIFO
        | LANDLOCK_ACCESS_FS_MAKE_BLOCK
        | LANDLOCK_ACCESS_FS_MAKE_SYM;
#if defined( LANDLOCK_ACCESS_FS_REFER )
    if ( abiVersion >= 2 ) {
        handledAccess |= LANDLOCK_ACCESS_FS_REFER;
    }
#endif
#if defined( LANDLOCK_ACCESS_FS_TRUNCATE )
    if ( abiVersion >= 3 ) {
        handledAccess |= LANDLOCK_ACCESS_FS_TRUNCATE;
    }
#endif
#if defined( LANDLOCK_ACCESS_FS_IOCTL_DEV )
    if ( abiVersion >= 5 ) {
        handledAccess |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    }
#endif

    struct landlock_ruleset_attr rulesetAttribute{};
    rulesetAttribute.handled_access_fs = handledAccess;
    const int rulesetFd = static_cast<int>(
        syscall( __NR_landlock_create_ruleset, &rulesetAttribute, sizeof( rulesetAttribute ), 0u ) );
    if ( rulesetFd < 0 ) {
        const int errorCode = errno;
        if ( ( errorCode == ENOSYS ) || ( errorCode == EOPNOTSUPP ) ) {
            if ( config.verbose && !config.quiet ) {
                std::cerr << "[Info] Landlock is not available. Continuing without it.\n";
            }
        } else {
            warn( config, "creating a Landlock ruleset failed", errorCode );
        }
        return;
    }

    for ( const auto& path : config.readablePaths ) {
        if ( path.empty() ) {
            continue;
        }

        const int pathFd = open( path.c_str(), O_PATH | O_CLOEXEC );
        if ( pathFd < 0 ) {
            const int errorCode = errno;
            warn( config, "opening the readable path '" + path + "' for Landlock failed", errorCode );
            continue;
        }

        struct landlock_path_beneath_attr pathRule{};
        pathRule.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE;
        pathRule.parent_fd = pathFd;
        if ( syscall( __NR_landlock_add_rule, rulesetFd, LANDLOCK_RULE_PATH_BENEATH, &pathRule, 0u ) != 0 ) {
            const int errorCode = errno;
            warn( config, "adding a Landlock rule for '" + path + "' failed", errorCode );
        }
        close( pathFd );
    }

    if ( syscall( __NR_landlock_restrict_self, rulesetFd, 0u ) != 0 ) {
        const int errorCode = errno;
        warn( config, "restricting the process with Landlock failed", errorCode );
    } else if ( config.verbose && !config.quiet ) {
        std::cerr << "[Info] Installed Landlock ABI v" << abiVersion << " filesystem sandbox.\n";
    }
    close( rulesetFd );
}
#else
[[nodiscard]] inline bool
landlockIsAvailable()
{
    return false;
}


inline void
installLandlockSandbox( const rapidgzip::sandbox::Config& config )
{
    if ( config.verbose && !config.quiet ) {
        std::cerr << "[Info] Landlock headers are not available. Continuing without it.\n";
    }
}
#endif


#endif


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
    installSeccompFilter( config );
    installLandlockSandbox( config );
}
#else
inline void
installIfEnabled( const Config& )
{}
#endif
}
