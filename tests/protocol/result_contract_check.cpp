#include <iostream>
#include <iterator>
#include <spectrapack/io/contracts.hpp>
#include <string>
#include <variant>

int main()
{
    const std::string input { std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>() };

    spectrapack::io::ContractValidator validator;
    const auto outcome = validator.parse(spectrapack::io::ContractKind::results, input);
    if (std::holds_alternative<spectrapack::io::ValidatedDocument>(outcome)) {
        return 0;
    }

    const auto& failure = std::get<spectrapack::io::ContractFailure>(outcome);
    std::cerr << "results contract validation failed with " << failure.issues.size() << " issue(s)\n";
    for (const auto& issue : failure.issues) {
        std::cerr << issue.path << ": " << issue.code << ": " << issue.message << '\n';
    }
    return 1;
}
