#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <Python.h>

#include <core/ScopedGIL.hpp>

#include "FileReader.hpp"


namespace rapidgzip
{
template<typename Value,
         typename std::enable_if_t<std::is_integral_v<Value> && std::is_signed_v<Value>, void*> = nullptr>
[[nodiscard]] PyObject*
toPyObject( Value value )
{
    auto* const result = PyLong_FromLongLong( value );
    if ( result == nullptr ) {
        throw std::runtime_error( "PyLong_FromLongLong returned null for: " + std::to_string( value ) + "!" );
    }
    return result;
}


template<typename Value,
         typename std::enable_if_t<std::is_integral_v<Value> && !std::is_signed_v<Value>, void*> = nullptr>
[[nodiscard]] PyObject*
toPyObject( Value value )
{
    auto* const result = PyLong_FromUnsignedLongLong( value );
    if ( result == nullptr ) {
        throw std::runtime_error( "PyLong_FromUnsignedLongLong returned null for: " + std::to_string( value ) + "!" );
    }
    return result;
}


[[nodiscard]] inline PyObject*
toPyObject( PyObject* value )
{
    if ( value == nullptr ) {
        throw std::runtime_error( "Got null PyObject as argument to toPyObject!" );
    }
    Py_INCREF( value );
    return value;
}


template<typename Result>
[[nodiscard]] Result
fromPyObject( PyObject* pythonObject );

/* For "long" objects, see https://docs.python.org/3/c-api/long.html */

template<>
[[nodiscard]] long long int
fromPyObject<long long int>( PyObject* pythonObject )
{
    /** @todo How to do better error reporting if the object can't be converted to the specified type? */
    return PyLong_AsLongLong( pythonObject );
}


template<>
[[nodiscard]] size_t
fromPyObject<size_t>( PyObject* pythonObject )
{
    return PyLong_AsSize_t( pythonObject );
}

/* For bool objects, see https://docs.python.org/3/c-api/long.html */

template<>
[[nodiscard]] bool
fromPyObject<bool>( PyObject* pythonObject )
{
    return pythonObject == Py_True;
}


template<>
[[nodiscard]] PyObject*
fromPyObject<PyObject*>( PyObject* pythonObject )
{
    return pythonObject;
}


[[nodiscard]] inline std::string
pythonErrorMessage()
{
    PyObject* type{ nullptr };
    PyObject* value{ nullptr };
    PyObject* traceback{ nullptr };
    PyErr_Fetch( &type, &value, &traceback );
    PyErr_NormalizeException( &type, &value, &traceback );

    std::string message;
    if ( value != nullptr ) {
        if ( auto* const string = PyObject_Str( value ); string != nullptr ) {
            Py_ssize_t size{ 0 };
            if ( const auto* const utf8 = PyUnicode_AsUTF8AndSize( string, &size ); utf8 != nullptr ) {
                message.assign( utf8, static_cast<size_t>( size ) );
            }
            Py_DECREF( string );
        }
    }

    Py_XDECREF( type );
    Py_XDECREF( value );
    Py_XDECREF( traceback );
    PyErr_Clear();
    return message;
}


/** Build an argument tuple from new references returned by toPyObject(). */
template<typename... Args>
[[nodiscard]] PyObject*
packPyArguments( Args... args )
{
    std::array<PyObject*, sizeof...( Args )> objects{ toPyObject( args )... };
    auto* const tuple = PyTuple_New( static_cast<Py_ssize_t>( objects.size() ) );
    if ( tuple == nullptr ) {
        for ( auto* const object : objects ) {
            Py_XDECREF( object );
        }
        return nullptr;
    }

    for ( size_t i = 0; i < objects.size(); ++i ) {
        if ( objects[i] == nullptr ) {
            Py_DECREF( tuple );
            for ( size_t j = i; j < objects.size(); ++j ) {
                Py_XDECREF( objects[j] );
            }
            return nullptr;
        }
        if ( PyTuple_SetItem( tuple, static_cast<Py_ssize_t>( i ), objects[i] ) != 0 ) {
            /* PyTuple_SetItem steals the reference on success and failure. */
            Py_DECREF( tuple );
            for ( size_t j = i + 1; j < objects.size(); ++j ) {
                Py_XDECREF( objects[j] );
            }
            return nullptr;
        }
    }

    return tuple;
}


template<typename Result,
         typename... Args>
Result
callPyObject( PyObject* pythonObject,
              Args...   args )
{
    if ( pythonObject == nullptr ) {
        throw std::invalid_argument( "[callPyObject] Got null PyObject!" );
    }

    const ScopedGILLock gilLock;

    auto* const arguments = packPyArguments( args... );
    if ( arguments == nullptr ) {
        throw std::invalid_argument( "[callPyObject] Failed to build the argument tuple: " + pythonErrorMessage() );
    }

    auto* const result = PyObject_Call( pythonObject, arguments, nullptr );
    Py_DECREF( arguments );

    if ( result == nullptr ) {
        std::stringstream message;
        message << "Python call failed";
    #if !defined( Py_LIMITED_API )
        if ( ( pythonObject != nullptr ) && ( pythonObject->ob_type != nullptr ) ) {
            message << " for " << pythonObject->ob_type->tp_name;
        }
    #endif
        const auto detail = pythonErrorMessage();
        if ( !detail.empty() ) {
            message << ": " << detail;
        }
        throw std::invalid_argument( std::move( message ).str() );
    }

    if constexpr ( std::is_same_v<Result, void> ) {
        Py_DECREF( result );
    } else if constexpr ( std::is_same_v<Result, PyObject*> ) {
        return result;
    } else {
        const auto converted = fromPyObject<Result>( result );
        Py_DECREF( result );
        return converted;
    }
}


class PythonFileReader :
    public FileReader
{
public:
    explicit
    PythonFileReader( PyObject* pythonObject ) :
        m_pythonObject( checkNullptr( pythonObject ) ),
        mpo_tell( getAttribute( m_pythonObject, "tell" ) ),
        mpo_seek( getAttribute( m_pythonObject, "seek" ) ),
        mpo_read( getAttribute( m_pythonObject, "read" ) ),
        mpo_write( getAttribute( m_pythonObject, "write" ) ),
        mpo_seekable( getAttribute( m_pythonObject, "seekable" ) ),
        mpo_close( getAttribute( m_pythonObject, "close" ) ),
        m_initialPosition( callPyObject<long long int>( mpo_tell ) ),
        m_seekable( callPyObject<bool>( mpo_seekable ) )
    {
        if ( m_seekable ) {
            m_fileSizeBytes = seek( 0, SEEK_END );
            seek( 0, SEEK_SET );
        }

        /* No throws should come after incrementing the reference count because an exception thrown inside the
         * constructor means the destructor will not be called! */
        Py_INCREF( m_pythonObject );
    }

    ~PythonFileReader()
    {
        close();
    }

    [[nodiscard]] UniqueFileReader
    cloneRaw() const override
    {
        throw std::invalid_argument( "Cloning file path reader not allowed because the internal file position "
                                     "should not be modified by multiple owners!" );
    }

    /* Copying is simply not allowed because that might interfere with the file position state, use SharedFileReader! */

    void
    close() override
    {
        if ( m_pythonObject == nullptr ) {
            return;
        }

        /* During interpreter finalization, acquiring the GIL is not possible anymore. Leaking the
         * references is safe because the process is shutting down. */
        if ( pythonIsFinalizing() ) {
            m_pythonObject = nullptr;
            return;
        }

        /* Try to restore the file position the file had before it was given to us. */
        if ( m_seekable ) {
            seek( m_initialPosition );
        }

        const ScopedGILLock gilLock;

        /* The bound methods keep the instance alive. Release all except close() first so that the
         * reference count tells us whether the caller still owns the object. */
        releaseNonLifetimeAttributes();

        if ( Py_REFCNT( m_pythonObject ) == 2 ) {
            callPyObject<void>( mpo_close );
        }

        Py_XDECREF( mpo_close );
        mpo_close = nullptr;
        Py_DECREF( m_pythonObject );
        m_pythonObject = nullptr;
    }

    [[nodiscard]] bool
    closed() const override
    {
        return m_pythonObject == nullptr;
    }

    [[nodiscard]] bool
    eof() const override
    {
        return m_seekable ? tell() >= size() : !m_lastReadSuccessful;
    }

    [[nodiscard]] bool
    fail() const override
    {
        return false;
    }

    [[nodiscard]] int
    fileno() const override
    {
        throw std::invalid_argument( "This Python file-like object has no valid fileno!" );
    }

    [[nodiscard]] bool
    seekable() const override
    {
        return m_seekable;
    }

    [[nodiscard]] size_t
    read( char*  buffer,
          size_t nMaxBytesToRead ) override
    {
        if ( m_pythonObject == nullptr ) {
            throw std::invalid_argument( "Invalid or file can't be read from!" );
        }

        if ( nMaxBytesToRead == 0 ) {
            return 0;
        }

        const ScopedGILLock gilLock;

        /** @todo better to use readinto because read might return less than requested even before the EOF! */
        auto* const bytes = callPyObject<PyObject*>( mpo_read, nMaxBytesToRead );
        if ( !PyBytes_Check( bytes ) ) {
            Py_XDECREF( bytes );
            throw std::runtime_error( "Expected a bytes object to be returned by read!" );
        }

        const auto nBytesAvailable = PyBytes_Size( bytes );
        if ( nBytesAvailable < 0 ) {
            Py_XDECREF( bytes );
            std::stringstream message;
            message
                << "[PythonFileReader] Read call failed (" << nBytesAvailable << " B read)!\n"
                << "  Buffer: " << (void*)buffer << "\n"
                << "  nMaxBytesToRead: " << nMaxBytesToRead << " B\n"
                << "  File size: " << m_fileSizeBytes << " B\n"
                << "  m_currentPosition: " << m_currentPosition << "\n"
                << "  tell: " << tell() << "\n"
                << "\n";
            std::cerr << message.str();
            throw std::domain_error( std::move( message ).str() );
        }

        /* Python's read(size) contract requires at most size bytes, but a file-like object may violate it.
         * Copying the returned length unconditionally would write past the caller's buffer. */
        if ( static_cast<size_t>( nBytesAvailable ) > nMaxBytesToRead ) {
            Py_XDECREF( bytes );
            std::stringstream message;
            message << "[PythonFileReader] The file-like object returned " << nBytesAvailable
                    << " B but only " << nMaxBytesToRead << " B were requested!";
            throw std::runtime_error( std::move( message ).str() );
        }

        const auto nBytesRead = static_cast<size_t>( nBytesAvailable );
        if ( buffer != nullptr ) {
            std::memcpy( buffer, PyBytes_AsString( bytes ), nBytesRead );
        }
        Py_XDECREF( bytes );

        m_currentPosition += nBytesRead;
        m_lastReadSuccessful = nBytesRead == nMaxBytesToRead;

        return nBytesRead;
    }

    /**
     * Should not be mixed with read calls on the same PythonFileReader object!
     */
    [[nodiscard]] size_t
    write( const char* buffer,
           size_t      nBytesToWrite )
    {
        if ( m_pythonObject == nullptr ) {
            throw std::invalid_argument( "Invalid or file can't be written to!" );
        }

        if ( nBytesToWrite == 0 ) {
            return 0;
        }

        const ScopedGILLock gilLock;

        auto* const bytes = PyBytes_FromStringAndSize( buffer, nBytesToWrite );
        long long int nBytesWritten{ -1 };
        try {
            nBytesWritten = callPyObject<long long int>( mpo_write, bytes );
        } catch ( ... ) {
            Py_XDECREF( bytes );
            throw;
        }
        Py_XDECREF( bytes );

        if ( ( nBytesWritten < 0 ) || ( static_cast<size_t>( nBytesWritten ) < nBytesToWrite ) ) {
            std::stringstream message;
            message
                << "[PythonFileReader] Write call failed (" << nBytesWritten << " B written)!\n"
                << "  Buffer: " << (void*)buffer << "\n"
                << "  tell: " << tell() << "\n"
                << "\n";
            std::cerr << message.str();
            throw std::domain_error( std::move( message ).str() );
        }

        return static_cast<size_t>( nBytesWritten );
    }

    size_t
    seek( long long int offset,
          int           origin = SEEK_SET ) override
    {
        if ( ( m_pythonObject == nullptr ) || !m_seekable ) {
            throw std::invalid_argument( "Invalid or unseekable file can't be seeked!" );
        }

        /* see https://docs.python.org/3/library/io.html#io.IOBase.seek */
        int pythonWhence = 0;
        switch ( origin )
        {
        case SEEK_SET:
            pythonWhence = 0;
            break;
        case SEEK_CUR:
            pythonWhence = 1;
            break;
        case SEEK_END:
            pythonWhence = 2;
            break;
        }

        m_currentPosition = callPyObject<size_t>( mpo_seek, offset, pythonWhence );

        return m_currentPosition;
    }

    [[nodiscard]] std::optional<size_t>
    size() const override
    {
        return m_seekable ? std::optional<size_t>{ m_fileSizeBytes } : std::nullopt;
    }

    [[nodiscard]] size_t
    tell() const override
    {
        if ( m_pythonObject == nullptr ) {
            throw std::invalid_argument( "Cannot call 'tell' on invalid file!" );
        }
        return callPyObject<size_t>( mpo_tell );
    }

    void
    clearerr() override
    {
        /* There exists nothing like this for Python's RawIOBase */
    }

private:
    [[nodiscard]] static PyObject*
    checkNullptr( PyObject* pythonObject )
    {
        if ( pythonObject == nullptr ) {
            throw std::invalid_argument( "PythonFileReader may not be constructed with a nullptr PyObject!" );
        }
        return pythonObject;
    }

    [[nodiscard]] static PyObject*
    getAttribute( PyObject*   pythonObject,
                  const char* name )
    {
        auto* const attribute = PyObject_GetAttrString( pythonObject, name );

        if ( attribute == nullptr ) {
            std::stringstream message;
            message << "The given Python file-like object must have a '" << name << "' method!";
            throw std::invalid_argument( std::move( message ).str() );
        }

        return attribute;
    }

    void
    releaseNonLifetimeAttributes() noexcept
    {
        Py_XDECREF( mpo_tell );
        mpo_tell = nullptr;
        Py_XDECREF( mpo_seek );
        mpo_seek = nullptr;
        Py_XDECREF( mpo_read );
        mpo_read = nullptr;
        Py_XDECREF( mpo_write );
        mpo_write = nullptr;
        Py_XDECREF( mpo_seekable );
        mpo_seekable = nullptr;
    }

protected:
    PyObject* m_pythonObject{ nullptr };
    PyObject* mpo_tell{ nullptr };
    PyObject* mpo_seek{ nullptr };
    PyObject* mpo_read{ nullptr };
    PyObject* mpo_write{ nullptr };
    PyObject* mpo_seekable{ nullptr };
    PyObject* mpo_close{ nullptr };

    const long long int m_initialPosition;
    const bool m_seekable;
    size_t m_fileSizeBytes{ 0 };

    size_t m_currentPosition{ 0 };  /**< Only necessary for unseekable files. */
    bool m_lastReadSuccessful{ true };
};
}  // namespace rapidgzip
