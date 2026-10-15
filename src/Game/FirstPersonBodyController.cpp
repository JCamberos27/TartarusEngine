// The sample body graph is authored as a project template; native code fills its clip roles and decodes it.
#include "FirstPersonBodyContract.h"
#include "AnimatorController.h"
#include "ProjectPaths.h"
#include <json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
namespace FPBody {
namespace {
using Json = nlohmann::ordered_json; // keeps the template's order, so the roles list reads as authored
Json Template() {
    std::ifstream in(ProjectPaths::Resolve("assets/Animations/Controllers/fps_body_locomotion.template.json"));
    if(!in) throw std::runtime_error("Body locomotion template is missing");
    return Json::parse(in);
}
// Every object in the template that names a clip role, depth first.
template<class F> void ForEachClip(Json& node, const F& visit) {
    if(node.is_object()) {
        if(auto it=node.find("clip"); it!=node.end() && it->is_string() && !it->get_ref<const std::string&>().empty()) visit(node);
        for(auto& item:node.items()) ForEachClip(item.value(),visit);
    } else if(node.is_array()) for(auto& child:node) ForEachClip(child,visit);
}
bool SameText(const std::string& a, const std::string& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](unsigned char x,unsigned char y){return std::tolower(x)==std::tolower(y);});
}
}
std::vector<std::string> LocomotionRoles() {
    std::vector<std::string> roles; Json graph=Template();
    ForEachClip(graph,[&](Json& o){const auto& role=o["clip"].get_ref<const std::string&>();if(std::find(roles.begin(),roles.end(),role)==roles.end())roles.push_back(role);});
    return roles;
}
AnimatorController BuildLocomotionController(const std::function<std::string(const std::string&)>& clipForRole) {
    Json graph=Template();
    ForEachClip(graph,[&](Json& o){const std::string role=o["clip"].get<std::string>();o["clip"]=clipForRole?clipForRole(role):std::string{};});
    AnimatorController controller;if(!AnimatorController::FromJsonString(graph.dump(),controller))throw std::runtime_error("Invalid body locomotion graph");return controller;
}
std::string PickLocomotionClip(const std::string& role,const std::vector<std::string>& files) {
    for(const auto& file:files) {
        std::string normalized=file;std::replace(normalized.begin(),normalized.end(),'\\','/');
        std::string stem=std::filesystem::path(normalized).stem().string();
        if(stem.size()>=3 && SameText(stem.substr(0,3),"AM_")) stem=stem.substr(3);
        if(SameText(stem,role)) return file;
    }
    return {};
}
}
