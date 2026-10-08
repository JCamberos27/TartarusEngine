// Sample graph authoring belongs to the project; native code only decodes the generic graph.
#include "FirstPersonBodyContract.h"
#include "AnimatorController.h"
#include "Scripting/ScriptRuntime.h"
#include <json.hpp>
#include <stdexcept>
namespace FPBody {
namespace {
std::string ProjectGraph(const char* operation,const nlohmann::json& data) {
    std::string result;if(!Scripting::RequestProject(operation,data.dump(),result))throw std::runtime_error("Project body authoring unavailable");return result;
}
}
std::vector<std::string> LocomotionRoles() {return nlohmann::json::parse(ProjectGraph("body.roles",{})).get<std::vector<std::string>>();}
AnimatorController BuildLocomotionController(const std::function<std::string(const std::string&)>& clipForRole) {
    nlohmann::json clips=nlohmann::json::object();for(const auto& role:LocomotionRoles())clips[role]=clipForRole?clipForRole(role):std::string{};
    AnimatorController controller;if(!AnimatorController::FromJsonString(ProjectGraph("body.graph",clips),controller))throw std::runtime_error("Invalid project body graph");return controller;
}
std::string PickLocomotionClip(const std::string& role,const std::vector<std::string>& files) {return nlohmann::json::parse(ProjectGraph("body.pick",{{"role",role},{"files",files}})).get<std::string>();}
}
