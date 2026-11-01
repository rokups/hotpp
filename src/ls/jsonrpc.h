// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The base protocol of LSP: "Content-Length: N\r\n\r\n" and N bytes of JSON.
#pragma once

#include <string>

namespace hot::ls
{

// Reads the messages from a file descriptor. One reader for each descriptor.
class MessageReader
{
public:
    explicit MessageReader(int fd)
        : fd_(fd)
    {
    }
    // Reads one message body. Returns false at the end of the input or on a bad header.
    bool read(std::string& body);

private:
    bool fill(); // reads more bytes into buf_; false at the end of the input
    int fd_;
    std::string buf_;
};

// Writes one message with its header. Returns false when the write fails.
// The caller makes sure that only one thread writes to the descriptor at a time.
bool write_message(int fd, const std::string& body);

} // namespace hot::ls
