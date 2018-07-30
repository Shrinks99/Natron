/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://natrongithub.github.io/>,
 * Copyright (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "MemoryFile.h"

#ifdef __NATRON_WIN32__
# include <windows.h>
#else // unix
      //# include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>      // mmap, munmap.
#include <sys/stat.h>
#include <sys/types.h>     // struct stat.
#include <unistd.h>        // sysconf.
#include <cstring>
#include <cerrno>
#include <cstdio>
#endif
#include <sstream> // stringstream
#include <iostream>
#include <cassert>
#include <stdexcept>

#include <QMutex>

#include "Global/GlobalDefines.h"
#include "Global/StrUtils.h"

#define MIN_FILE_SIZE 4096

NATRON_NAMESPACE_ENTER

struct MemoryFilePrivate
{
    std::string path; //< filepath of the backing file

    char* data; //< pointer to the begining of the mapped file
    size_t size; //< the effective size of the file
#if defined(__NATRON_UNIX__)
    int file_handle; //< unix file handle
#elif defined(__NATRON_WIN32__)
    HANDLE file_handle; //< windows file handle
    HANDLE file_mapping_handle; //< windows memory mapped handle
#else
#error Only Unix or Windows systems can use memory-mapped files.
#endif

    MemoryFilePrivate(const std::string & filepath)
        : path(filepath)
        , data(0)
        , size(0)
#if defined(__NATRON_UNIX__)
        , file_handle(-1)
#elif defined(__NATRON_WIN32__)
        , file_handle(INVALID_HANDLE_VALUE)
        , file_mapping_handle(INVALID_HANDLE_VALUE)
#endif
    {
    }

    void openInternal(MemoryFile::FileOpenModeEnum open_mode);

    void closeMapping();
};

MemoryFile::MemoryFile()
    : _imp( new MemoryFilePrivate( std::string() ) )
{
}

void
MemoryFile::open(const std::string & filepath,
                 FileOpenModeEnum open_mode)
{
    if (_imp->data) {
        return;
    }
    _imp->path = filepath;
    _imp->openInternal(open_mode);
}

void
MemoryFilePrivate::openInternal(MemoryFile::FileOpenModeEnum open_mode)
{
#if defined(__NATRON_UNIX__)
    /*********************************************************
     ********************************************************

       CHOOSING FILE OPEN MODE
     ********************************************************
     *********************************************************/
    int posix_open_mode = O_RDWR;
    switch (open_mode) {
    case MemoryFile::eFileOpenModeCreate:
        posix_open_mode |= O_EXCL | O_CREAT;
        break;
    case MemoryFile::eFileOpenModeOpen:
        break;
    case MemoryFile::eFileOpenModeOpenOrCreate:
        posix_open_mode |= O_CREAT;
        break;
    case MemoryFile::eFileOpenModeOpenTruncate:
        posix_open_mode |= O_TRUNC;
        break;
    case MemoryFile::eFileOpenModeOpenTruncateOrCreate:
        posix_open_mode |= O_TRUNC | O_CREAT;
        break;
    default:

        return;
    }

    /*********************************************************
     ********************************************************

       OPENING THE FILE WITH RIGHT PERMISSIONS:
       - R/W user
       - R Group
       - R Other
     ********************************************************
     *********************************************************/
    file_handle = ::open(path.c_str(), posix_open_mode, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (file_handle == -1) {
        std::stringstream ss;
        ss << "MemoryFile EXC : Failed to open \"" << path << "\": " << std::strerror(errno) << " (" << errno << ")";
        throw std::runtime_error( ss.str() );
    }

    /*********************************************************
     ********************************************************

       GET FILE SIZE
     ********************************************************
     *********************************************************/
    struct stat sbuf;
    if (::fstat(file_handle, &sbuf) == -1) {
        std::stringstream ss;
        ss << "MemoryFile EXC : Failed to get file info \"" << path << "\": " << std::strerror(errno) << " (" << errno << ")";
        throw std::runtime_error( ss.str() );
    }

    /*********************************************************
     ********************************************************

       IF FILE IS NOT EMPTY,  MMAP IT
     ********************************************************
     *********************************************************/
    if (sbuf.st_size > 0) {
        data = static_cast<char*>( ::mmap(
                                       0, sbuf.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, file_handle, 0) );
        if (data == MAP_FAILED) {
            data = 0;
            std::stringstream ss;
            ss << "MemoryFile EXC : Failed to create mapping for \"" << path << "\": " << std::strerror(errno) << " (" << errno << ")";
            throw std::runtime_error( ss.str() );
        } else {
            size = sbuf.st_size;
        }
    }
#elif defined(__NATRON_WIN32__)

    /*********************************************************
     ********************************************************

       CHOOSING FILE OPEN MODE
     ********************************************************
     *********************************************************/
    int windows_open_mode;
    switch (open_mode) {
    case MemoryFile::eFileOpenModeCreate:
        windows_open_mode = CREATE_NEW;
        break;
    case MemoryFile::eFileOpenModeOpen:
        windows_open_mode = OPEN_EXISTING;
        break;
    case MemoryFile::eFileOpenModeOpenOrCreate:
        windows_open_mode = OPEN_ALWAYS;
        break;
    case MemoryFile::eFileOpenModeOpenTruncate:
        windows_open_mode = TRUNCATE_EXISTING;
        break;
    case MemoryFile::eFileOpenModeOpenTruncateOrCreate:
        windows_open_mode = CREATE_ALWAYS;
        break;
    default:
        std::string str("MemoryFile EXC : Invalid open mode. ");
        str.append(path);
        throw std::runtime_error(str);

        return;
    }
    /*********************************************************
     ********************************************************

       OPENING THE FILE WITH RIGHT PERMISSIONS:
       - R/W
     ********************************************************
     *********************************************************/
    std::wstring wpath = StrUtils::utf8_to_utf16(path);
    file_handle = ::CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0, windows_open_mode, FILE_ATTRIBUTE_NORMAL, 0);


    if (file_handle == INVALID_HANDLE_VALUE) {
        std::string winError = StrUtils::GetLastErrorAsString();
        std::string str("MemoryFile EXC : Failed to open file ");
        str.append(path);
        str.append(winError);
        throw std::runtime_error(str);
    }

    /*********************************************************
     ********************************************************

       GET FILE SIZE
     ********************************************************
     *********************************************************/
    size_t fileSize = ::GetFileSize(file_handle, 0);

    /*********************************************************
     ********************************************************

       IF FILE IS NOT EMPTY,  MMAP IT
     ********************************************************
     *********************************************************/
    if (fileSize > 0) {
        file_mapping_handle = ::CreateFileMapping(file_handle, 0, PAGE_READWRITE, 0, 0, 0);
        data = static_cast<char*>( ::MapViewOfFile(file_mapping_handle, FILE_MAP_WRITE, 0, 0, 0) );
        if (data) {
            size = fileSize;
        } else {
            throw std::runtime_error("MemoryFile EXC : Failed to create mapping.");
        }
    }

#endif // if defined(__NATRON_UNIX__)
} // openInternal

char*
MemoryFile::getData() const
{
    return _imp->data;
}

size_t
MemoryFile::size() const
{
    return _imp->size;
}

std::string
MemoryFile::path() const
{
    return _imp->path;
}

void
MemoryFile::remap()
{
    if (!_imp->data) {
        return;
    }
    // Sync the content to the file
    flush(eFlushTypeSync, NULL, 0);

    // Close the mapping
    _imp->closeMapping();

    // re-open it
    _imp->openInternal(eFileOpenModeOpen);
}

void
MemoryFile::resize(size_t new_size, bool preserve)
{
    // Before unmapping, flush to avoid expensive copy if the user does not want to preserve the data
    if (preserve) {
        flush(eFlushTypeSync, _imp->data, _imp->size);
    }
#if defined(__NATRON_UNIX__)
    if (_imp->data) {
        if (::munmap(_imp->data, _imp->size) < 0) {
            std::stringstream ss;
            ss << "MemoryFile EXC : Failed to unmap \"" << _imp->path << "\": " << std::strerror(errno) << " (" << errno << ")";
            throw std::runtime_error( ss.str() );
        }
    }
    if (::ftruncate(_imp->file_handle, new_size) < 0) {
        std::stringstream ss;
        ss << "MemoryFile EXC : Failed to truncate the file \"" << _imp->path << "\": " << std::strerror(errno) << " (" << errno << ")";
        throw std::runtime_error( ss.str() );
    }
    _imp->data = static_cast<char*>( ::mmap(
                                         0, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, _imp->file_handle, 0) );
    if (_imp->data == MAP_FAILED) {
        _imp->data = 0;
        std::stringstream ss;
        ss << "MemoryFile EXC : Failed to create mapping of \"" << _imp->path << "\": " << std::strerror(errno) << " (" << errno << ")";
        throw std::runtime_error( ss.str() );
    }

#elif defined(__NATRON_WIN32__)

    ::UnmapViewOfFile(_imp->data);
    ::CloseHandle(_imp->file_mapping_handle);
    _imp->file_mapping_handle = ::CreateFileMapping(
        _imp->file_handle, 0, PAGE_READWRITE, 0, new_size, 0);
    _imp->data = static_cast<char*>( ::MapViewOfFile(
                                         _imp->file_mapping_handle, FILE_MAP_WRITE, 0, 0, 0) );
#endif

    if (!_imp->data) {
        throw std::bad_alloc();
    }
    _imp->size = new_size;
}

void
MemoryFilePrivate::closeMapping()
{
#if defined(__NATRON_UNIX__)
    if (data) {
        ::munmap(data, size);
    }
    if (file_handle != -1) {
        ::close(file_handle);
    }
    file_handle = -1;
#elif defined(__NATRON_WIN32__)
    if (data) {
        ::UnmapViewOfFile(data);
    }
    if (file_mapping_handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(file_mapping_handle);
    }
    if (file_handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(file_handle);
    }
    file_handle = INVALID_HANDLE_VALUE;
    file_mapping_handle = INVALID_HANDLE_VALUE;
#endif
    data = 0;
    size = 0;

} // closeMapping

bool
MemoryFile::flush(FlushTypeEnum type, void* data, std::size_t size)
{
    void* ptr = data ? data : _imp->data;
    if (!ptr) {
        return true;
    }
    std::size_t n = data ? size : _imp->size;
#if defined(__NATRON_UNIX__)
    switch (type) {
        case eFlushTypeAsync:
            return ::msync(ptr, n, MS_ASYNC) == 0;
        case eFlushTypeSync:
            return ::msync(ptr, n, MS_SYNC) == 0;
        case eFlushTypeInvalidate: {
            int rc;
#ifdef MS_KILLPAGES
            // Mac OS X always returns an error with MADV_FREE,
            // so we'll use msync(MS_KILLPAGES) instead.
            rc = msync(data, size, MS_KILLPAGES);
            //if( rc ) return ERR_MEM_STR_NUM("drop_pages(MS_KILLPAGES) failed", rc);
#else
# ifdef MADV_FREE
            rc = madvise(data, size, MADV_FREE);
            //if( rc ) return ERR_MEM_STR_NUM("drop_pages(MADV_FREE) failed", rc);
# else
#  ifdef POSIX_MADV_DONTNEED
            // we just hope that DONTNEED will actually free
            // the pages...
            rc = posix_madvise(data, size, POSIX_MADV_DONTNEED);
            //if( rc ) return ERR_MEM_STR_NUM("drop_pages(POSIX_MADV_DONTNEED) failed", rc);
#  else
            rc = madvise(data, size, MADV_DONTNEED);
            //if( rc ) return ERR_MEM_STR_NUM("drop_pages(MADV_DONTNEED) failed", rc);
            // end if POSIX_MADV_DONTNEED
#  endif
            // end if MADV_FREE
# endif
            // end if MS_KILLPAGES
#endif
            Q_UNUSED(rc);

        }
        // MS_INVALIDATE of msync seems of no-use, see discussion here
        // https://groups.google.com/forum/#!topic/comp.unix.programmer/reGmzSHKBHETE
        //return ::msync(ptr, n, MS_INVALIDATE) == 0;
        break;
    }
#elif defined(__NATRON_WIN32__)
    switch (type) {
        case eFlushTypeSync: {
            bool ret =  (bool)::FlushViewOfFile(ptr, n) != 0;
            if (ret) {
                ret = (bool)::FlushFileBuffers(_imp->file_handle);
            }
            return ret;
        } break;
        case eFlushTypeAsync:
            return (bool)::FlushViewOfFile(ptr, n) != 0;
            break;
        case eFlushTypeInvalidate:
            break;
    }
#endif
    return false;
} // flush

void
MemoryFile::close()
{
    _imp->closeMapping();
}

MemoryFile::~MemoryFile()
{
    if (_imp->data) {
        close();
    }
    delete _imp;
}

void
MemoryFile::remove()
{
    if ( !_imp->path.empty() ) {
        if (_imp->data) {
            // Invalidate the whole memory portion
            flush(eFlushTypeInvalidate, NULL, 0);
            _imp->closeMapping();
        }
        int ok = ::remove( _imp->path.c_str() );
        Q_UNUSED(ok);
        _imp->path.clear();
        _imp->data = 0;
    }
}

NATRON_NAMESPACE_EXIT

