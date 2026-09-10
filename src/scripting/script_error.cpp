#include "quantModeling/scripting/script_error.hpp"

#include <string>

namespace quantModeling::scripting
{

    std::string source_line_at(std::string_view source, std::size_t line)
    {
        if (line == 0)
            return {};
        std::size_t start = 0;
        std::size_t current = 1;
        while (start < source.size() && current < line)
        {
            if (source[start] == '\n')
                ++current;
            ++start;
        }
        if (current != line)
            return {};
        std::size_t end = start;
        while (end < source.size() && source[end] != '\n' && source[end] != '\r')
            ++end;
        return std::string(source.substr(start, end - start));
    }

    std::string ScriptError::format(const std::string &message, std::size_t line,
                                    std::size_t col,
                                    const std::string &source_line)
    {
        std::string out = "line " + std::to_string(line) + ", col " +
                          std::to_string(col) + ": " + message;
        if (!source_line.empty())
        {
            out += '\n';
            out += "    ";
            out += source_line;
            out += '\n';
            out += "    ";
            out += std::string(col > 0 ? col - 1 : 0, ' ');
            out += '^';
        }
        return out;
    }

} // namespace quantModeling::scripting
