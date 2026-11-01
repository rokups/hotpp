// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "jsonrpc.h"

#include <cerrno>
#include <cstdlib>
#ifdef _WIN32
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace hot::ls
{

namespace
{
long long read_fd(int fd, char* data, size_t size)
{
#ifdef _WIN32
    return ::_read(fd, data, static_cast<unsigned>(size));
#else
    return ::read(fd, data, size);
#endif
}

long long write_fd(int fd, const char* data, size_t size)
{
#ifdef _WIN32
    return ::_write(fd, data, static_cast<unsigned>(size));
#else
    return ::write(fd, data, size);
#endif
}
} // namespace

bool MessageReader::fill()
{
    char chunk[65536];
    for (;;)
    {
        const long long n = read_fd(fd_, chunk, sizeof chunk);
        if (n > 0)
        {
            buf_.append(chunk, static_cast<size_t>(n));
            return true;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
}

bool MessageReader::read(std::string& body)
{
    size_t end;
    while ((end = buf_.find("\r\n\r\n")) == std::string::npos)
        if (!fill())
            return false;
    // The header fields. Only Content-Length has a meaning here.
    long length = -1;
    size_t at = 0;
    while (at < end)
    {
        size_t eol = buf_.find("\r\n", at);
        if (eol == std::string::npos || eol > end)
            eol = end;
        const std::string field = buf_.substr(at, eol - at);
        static const std::string kLength = "Content-Length:";
        if (field.compare(0, kLength.size(), kLength) == 0)
            length = std::strtol(field.c_str() + kLength.size(), nullptr, 10);
        at = eol + 2;
    }
    if (length < 0)
        return false;
    const size_t begin = end + 4;
    while (buf_.size() < begin + static_cast<size_t>(length))
        if (!fill())
            return false;
    body = buf_.substr(begin, static_cast<size_t>(length));
    buf_.erase(0, begin + static_cast<size_t>(length));
    return true;
}

bool write_message(int fd, const std::string& body)
{
    const std::string msg = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t done = 0;
    while (done < msg.size())
    {
        const long long n = write_fd(fd, msg.data() + done, msg.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

} // namespace hot::ls
