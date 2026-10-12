// Stand-ins for glslang's SPIRV/SpvTools.cpp, which wraps SPIRV-Tools
// (validation, optimisation, disassembly).  gq_glsl never asks for any of it
// (SpvOptions::disableOptimizer, no validate, no disassemble), so these are
// never called; defining them here keeps libgq_glsl.so from depending on the
// SPIRV-Tools shared libraries, which a Steam Frame does not ship.
//
// The signatures only have to mangle like the real ones, so the types are
// forward declared instead of pulling in glslang's internal headers.
#include <ostream>
#include <unordered_set>
#include <vector>

enum spv_target_env : int;
enum spv_message_level_t : int;
struct spv_position_t;

namespace spv {
class SpvBuildLogger;
}

namespace glslang {
class TIntermediate;
struct SpvVersion;
struct SpvOptions;

spv_target_env MapToSpirvToolsEnv(const SpvVersion&, spv::SpvBuildLogger*) { return (spv_target_env)0; }
bool SpirvToolsValidate(const TIntermediate&, std::vector<unsigned int>&, spv::SpvBuildLogger*, bool) { return true; }
void SpirvToolsTransform(const TIntermediate&, std::vector<unsigned int>&, spv::SpvBuildLogger*, const SpvOptions*) {}
void SpirvToolsDisassemble(std::ostream&, const std::vector<unsigned int>&) {}
void SpirvToolsDisassemble(std::ostream&, const std::vector<unsigned int>&, spv_target_env) {}
void SpirvToolsStripDebugInfo(const TIntermediate&, std::vector<unsigned int>&, spv::SpvBuildLogger*) {}
void OptimizerMesssageConsumer(spv_message_level_t, const char*, const spv_position_t&, const char*) {}
void SpirvToolsAnalyzeDeadOutputStores(spv_target_env, std::vector<unsigned int>&, std::unordered_set<unsigned int>*,
                                       std::unordered_set<unsigned int>*, spv::SpvBuildLogger*) {}
void SpirvToolsEliminateDeadOutputStores(spv_target_env, std::vector<unsigned int>&, std::unordered_set<unsigned int>*,
                                         std::unordered_set<unsigned int>*, spv::SpvBuildLogger*) {}
void SpirvToolsEliminateDeadInputComponents(spv_target_env, std::vector<unsigned int>&, spv::SpvBuildLogger*) {}
}  // namespace glslang
