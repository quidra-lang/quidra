#include "optimization_cli.hpp"

#include <stdexcept>

namespace quidra::cli {
std::string_view optimization_key_name(OptimizationKey key) {
    switch (key) {
    case OptimizationKey::CfgFacts: return "cfg-facts";
    case OptimizationKey::ExactBinding: return "exact-binding";
    case OptimizationKey::Interproc: return "interproc";
    case OptimizationKey::Inline: return "inline";
    case OptimizationKey::CheckElimination: return "check-elimination";
    case OptimizationKey::StepRegions: return "step-regions";
    }
    throw std::invalid_argument("unknown typed optimization key");
}
std::string_view optimization_mode_name(ir::OptimizerMode mode) {
    switch (mode) {
    case ir::OptimizerMode::Off: return "off";
    case ir::OptimizerMode::Verify: return "verify";
    case ir::OptimizerMode::On: return "on";
    }
    throw std::invalid_argument("unknown typed optimization mode");
}
bool parse_optimization_option(std::string_view argument, OptimizationSettings& settings) {
    if (argument != "--opt" && !argument.starts_with("--opt=")) return false;
    if (!argument.starts_with("--opt="))
        throw std::runtime_error("--opt requires --opt=<key>=off|verify|on");
    const auto value = argument.substr(6);
    const auto separator = value.find('=');
    if (separator == std::string_view::npos)
        throw std::runtime_error("--opt requires --opt=<key>=off|verify|on");
    const auto key_text = value.substr(0, separator);
    const auto mode_text = value.substr(separator + 1);
    OptimizationKey key;
    if (key_text == "cfg-facts") key = OptimizationKey::CfgFacts;
    else if (key_text == "exact-binding") key = OptimizationKey::ExactBinding;
    else if (key_text == "interproc") key = OptimizationKey::Interproc;
    else if (key_text == "inline") key = OptimizationKey::Inline;
    else if (key_text == "check-elimination") key = OptimizationKey::CheckElimination;
    else if (key_text == "step-regions") key = OptimizationKey::StepRegions;
    else throw std::runtime_error("unknown optimization key: " + std::string(key_text));
    ir::OptimizerMode mode;
    if (mode_text == "off") mode = ir::OptimizerMode::Off;
    else if (mode_text == "verify") mode = ir::OptimizerMode::Verify;
    else if (mode_text == "on") mode = ir::OptimizerMode::On;
    else throw std::runtime_error("unknown optimization mode: " + std::string(mode_text));
    if (!settings.emplace(key, mode).second)
        throw std::runtime_error("duplicate optimization key: " + std::string(key_text));
    return true;
}
void apply_optimization_settings(const OptimizationSettings& settings, ir::OptimizerOptions& options) {
    for (const auto& [key, mode] : settings) {
        switch (key) {
        case OptimizationKey::CfgFacts: options.cfg_facts = mode; break;
        case OptimizationKey::ExactBinding:
            switch (mode) {
            case ir::OptimizerMode::Off: options.exact_binding = DeclarationBindingMode::Off; break;
            case ir::OptimizerMode::Verify: options.exact_binding = DeclarationBindingMode::Verify; break;
            case ir::OptimizerMode::On: options.exact_binding = DeclarationBindingMode::On; break;
            }
            break;
        case OptimizationKey::Interproc: options.interproc = mode; break;
        case OptimizationKey::Inline: options.inline_functions = mode; break;
        case OptimizationKey::CheckElimination: options.check_elimination = mode; break;
        case OptimizationKey::StepRegions: options.step_regions = mode; break;
        }
    }
    if (options.interproc == ir::OptimizerMode::On && options.exact_binding != DeclarationBindingMode::On)
        throw std::runtime_error("interproc=on requires exact-binding=on");
}
std::vector<std::string> normalized_optimization_options(const OptimizationSettings& settings) {
    std::vector<std::string> result; result.reserve(settings.size());
    for (const auto& [key, mode] : settings)
        result.push_back("--opt=" + std::string(optimization_key_name(key)) + "=" + std::string(optimization_mode_name(mode)));
    return result;
}
} // namespace quidra::cli
