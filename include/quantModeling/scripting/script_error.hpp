#ifndef QM_SCRIPTING_SCRIPT_ERROR_HPP
#define QM_SCRIPTING_SCRIPT_ERROR_HPP

#include "quantModeling/core/types.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace quantModeling::scripting
{

    /// The text of 1-based `line` in `source`, stripped of its line ending;
    /// empty when out of range. Shared by the lexer and parser to build the
    /// pointed extract in a ScriptError.
    std::string source_line_at(std::string_view source, std::size_t line);

    /**
     * @brief A malformed script: a user error, carried with a source location.
     *
     * Derives from PricingError so the existing FastAPI RequestValidationError
     * handler surfaces it verbatim (line, column, pointed extract) as a 422 —
     * a bad script must not look like an internal failure. See
     * blueprint/wp/16-scripting.md §3 and §8.3.
     */
    struct ScriptError : PricingError
    {
        ScriptError(const std::string &message, std::size_t at_line,
                    std::size_t at_col, std::string src_line)
            : PricingError(format(message, at_line, at_col, src_line)),
              line(at_line),
              col(at_col),
              source_line(std::move(src_line))
        {
        }

        std::size_t line;
        std::size_t col;
        std::string source_line;

      private:
        /// "col 6, line 3: <message>\n    <source line>\n         ^"
        static std::string format(const std::string &message, std::size_t line,
                                  std::size_t col, const std::string &source_line);
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_SCRIPT_ERROR_HPP
