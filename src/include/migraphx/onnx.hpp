#ifndef MIGRAPHX_GUARD_MIGRAPHLIB_ONNX_HPP
#define MIGRAPHX_GUARD_MIGRAPHLIB_ONNX_HPP

#include <migraphx/program.hpp>
#include <migraphx/config.hpp>
#include <sstream>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {

/// Create a program from an onnx file
program parse_onnx(const std::string& name);

program parse_model(const std::string& model_str, std::vector<std::string>& unsupported_nodes);

std::set<std::string> get_supported_ops();

} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx

#endif
