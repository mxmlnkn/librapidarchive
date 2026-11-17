#pragma once

#include <cstdint>
#include <optional>


namespace rapidgzip::blockfinder
{
class Interface
{
public:
    virtual
    ~Interface() = default;

    [[nodiscard]] virtual std::optional<uint64_t>
    find() = 0;
};
}  // rapidgzip::blockfinder
