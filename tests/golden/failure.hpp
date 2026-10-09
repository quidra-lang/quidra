// How the golden tool renders a compilation that throws: the exception class,
// every diagnostic's code, span and message, and a status for index.tsv
// (error:<first code>, error:logic_error, error:exception, error:unknown).
// An exception thrown as a class derived from the one its handler names
// (std::logic_error, std::exception) also shows its own class, `type=CLASS`,
// so a change of exception class shows in every view.
#pragma once

#include <string>

namespace quidra::golden {

struct Failure {
    std::string status;
    std::string text;
};

// Classifies the exception in flight. Call only from a catch handler.
Failure current_failure();

} // namespace quidra::golden
