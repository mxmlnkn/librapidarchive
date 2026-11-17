#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Sandbox.hpp"


namespace
{
[[nodiscard]] bool
runSandboxedChecks( const char* path )
{
    const int preOpenedFd = open( path, O_RDWR );
    if ( preOpenedFd < 0 ) {
        std::perror( "Opening the test file before the sandbox failed" );
        return false;
    }

    rapidgzip::sandbox::installIfEnabled( { { path }, /* verbose */ true, /* quiet */ false } );

    bool success = true;

    /* Files opened before the sandbox stay fully usable, including truncation. */
    if ( write( preOpenedFd, "x", 1 ) != 1 ) {
        std::perror( "Writing to a pre-opened file failed" );
        success = false;
    }
    if ( ftruncate( preOpenedFd, 0 ) != 0 ) {
        std::perror( "Truncating a pre-opened file failed" );
        success = false;
    }

    /* Paths passed as readable may still be reopened. */
    const int reopenedFd = open( path, O_RDONLY );
    if ( reopenedFd < 0 ) {
        std::perror( "Reopening an explicitly readable path failed" );
        success = false;
    } else {
        close( reopenedFd );
    }

    /* If Landlock is unavailable, the open may succeed. Do not fail in that case. */
    const bool landlockAvailable = rapidgzip::sandbox::landlockIsAvailable();
    if ( !landlockAvailable ) {
        std::fprintf( stderr, "Landlock is not available, skipping the path denial check.\n" );
    } else {
        errno = 0;
        const int otherFd = open( "/etc/passwd", O_RDONLY );
        if ( otherFd >= 0 ) {
            close( otherFd );
            std::fprintf( stderr, "Landlock is available but an unrelated path was opened!\n" );
            success = false;
        } else if ( errno != EACCES ) {
            std::perror( "Opening an unrelated path returned an unexpected error" );
            success = false;
        }
    }

    /* The syscall filter must block network access and executing another program. */
    errno = 0;
    const int socketFd = socket( AF_INET, SOCK_STREAM, 0 );
    if ( ( socketFd >= 0 ) || ( errno != EPERM ) ) {
        std::fprintf( stderr, "Creating a socket was not blocked with EPERM!\n" );
        success = false;
    }
    if ( socketFd >= 0 ) {
        close( socketFd );
    }

    errno = 0;
    char* const arguments[] = { const_cast<char*>( "/bin/true" ), nullptr };
    if ( ( execve( "/bin/true", arguments, nullptr ) != -1 ) || ( errno != EPERM ) ) {
        std::fprintf( stderr, "Executing a program was not blocked with EPERM!\n" );
        success = false;
    }

    /* Worker threads are created after the sandbox. They must work and inherit the restriction. */
    std::atomic<bool> threadOpenedPath{ false };
    std::thread thread(
        [&threadOpenedPath] {
            const int fd = open( "/etc/passwd", O_RDONLY );
            threadOpenedPath = fd >= 0;
            if ( fd >= 0 ) {
                close( fd );
            }
        } );
    thread.join();
    if ( landlockAvailable && threadOpenedPath ) {
        std::fprintf( stderr, "The Landlock restriction was not inherited by a worker thread!\n" );
        success = false;
    }

    close( preOpenedFd );
    return success;
}
}  // namespace


int
main()
{
    char path[] = "/tmp/rapidgzip-sandbox-test.XXXXXX";
    const int templateFd = mkstemp( path );
    if ( templateFd < 0 ) {
        std::perror( "Creating the test file failed" );
        return 1;
    }
    if ( write( templateFd, "hello", 5 ) != 5 ) {
        std::perror( "Writing the test file failed" );
        close( templateFd );
        unlink( path );
        return 1;
    }
    close( templateFd );

    /* The sandbox is irreversible and per-process, so run it in a child and clean up here. */
    const pid_t child = fork();
    if ( child < 0 ) {
        std::perror( "fork failed" );
        unlink( path );
        return 1;
    }

    if ( child == 0 ) {
        const bool success = runSandboxedChecks( path );
        _exit( success ? 0 : 1 );
    }

    int status = 0;
    if ( waitpid( child, &status, 0 ) != child ) {
        std::perror( "waitpid failed" );
        unlink( path );
        return 1;
    }
    unlink( path );

    if ( !WIFEXITED( status ) ) {
        std::fprintf( stderr, "The sandboxed child did not exit normally!\n" );
        return 1;
    }
    return WEXITSTATUS( status );
}
