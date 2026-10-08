#include "CurveEditor.h"
#include "AtomicFile.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <imgui_internal.h>

namespace CurveEditor {
namespace fs=std::filesystem;
using json=nlohmann::json;
struct AssetSource {
    std::string Path,Canonical;
    std::function<bool(const std::string&,std::string&)> Validate;
    std::function<void(const std::string&)> OnCommit;
    std::function<bool(Curve&,std::string&)> ReadCurve;
    std::function<bool(const Curve&,std::string&)> WriteCurve;
};
namespace {
thread_local AssetScope* activeScope=nullptr;
struct Binding {
    std::shared_ptr<AssetSource> Source;std::string Pointer,Title;
    Options Opt;Curve Default;std::string Format="%.3f";bool HasDefault=false;
};
struct Window {
    Binding Bound;Curve Working,Default;Options Opt;
    std::string Key,Format,Error;
    std::string SourceCurve;
    bool Focus=true,Dirty=false,Pending=false;
    fs::file_time_type Stamp{};
};
using Key=std::pair<ImGuiContext*,std::string>;
std::map<Key,Binding> bindings;
std::map<Key,Window> windows;
std::set<ImGuiContext*> contexts;
void ContextLifetime() {
    auto* ctx=ImGui::GetCurrentContext();if(!contexts.insert(ctx).second)return;
    ImGuiContextHook hook{};hook.Type=ImGuiContextHookType_Shutdown;
    hook.Callback=[](ImGuiContext* ctx,ImGuiContextHook*) {
        for(auto it=windows.begin();it!=windows.end();)if(it->first.first==ctx)it=windows.erase(it);else ++it;
        for(auto it=bindings.begin();it!=bindings.end();)if(it->first.first==ctx)it=bindings.erase(it);else ++it;
        contexts.erase(ctx);
    };
    ImGui::AddContextHook(ctx,&hook);
}
bool Document(const AssetSource& source,json& doc,std::string& error) {
    std::ifstream in(fs::u8path(source.Path));if(!in){error="Curve asset is unavailable.";return false;}
    doc=json::parse(std::string(std::istreambuf_iterator<char>(in),{}),nullptr,false);
    if(!doc.is_object()){error="Curve asset contains invalid JSON.";return false;}return true;
}
// @name selects array members by their names, keeping Animator windows safe across reordering.
bool Resolve(const json& doc,const std::string& path,json::json_pointer& result) {
    std::string resolved;const json* node=&doc;
    size_t begin=1;
    while(begin<=path.size()) {
        const auto end=path.find('/',begin);std::string token=path.substr(begin,end==std::string::npos?end:end-begin);
        std::string decoded=json::json_pointer("/"+token).back();
        if(!decoded.empty()&&decoded[0]=='@') {
            if(!node||!node->is_array())return false;
            size_t index=0;while(index<node->size()&&(!(*node)[index].is_object()||(*node)[index].value("name",std::string())!=decoded.substr(1)))++index;
            if(index==node->size())return false;token=std::to_string(index);
        }
        resolved+="/"+token;const json::json_pointer ptr(resolved);node=doc.contains(ptr)?&doc.at(ptr):nullptr;
        if(end==std::string::npos)break;begin=end+1;
    }
    result=json::json_pointer(resolved);return true;
}
bool Read(Window& w) {
    try {
    if(w.Bound.Source->ReadCurve) {
        if(!w.Bound.Source->ReadCurve(w.Working,w.Error))return false;
        w.SourceCurve=w.Working.ToJson(false).dump();
        w.Dirty=false;w.Error.clear();return true;
    }
    json doc;if(!Document(*w.Bound.Source,doc,w.Error))return false;
    const auto canonical=json::parse(w.Bound.Source->Canonical,nullptr,false);
    if(canonical.is_object()&&doc.value("version",0)!=canonical.value("version",0))doc=canonical;
    json::json_pointer ptr;
    if(!Resolve(doc,w.Bound.Pointer,ptr)){w.Error="This curve was removed or renamed.";return false;}
    if(!doc.contains(ptr))doc=json::parse(w.Bound.Source->Canonical,nullptr,false);
    if(!doc.contains(ptr)){w.Error="This curve no longer exists.";return false;}
    json keys=doc.at(ptr);
    if(w.Opt.LinearFallback&&keys.is_array())for(auto& key:keys)if(key.is_array()&&key.size()>=2&&key.size()<4) {
        while(key.size()<4)key.push_back(0);key.push_back(int(CurveInterpolation::Linear));
    }
    Curve c;if(!Curve::FromJson(keys,c)){w.Error="This curve contains invalid keys.";return false;}
    w.Working=std::move(c);if(w.Opt.TimeInSeconds)w.Opt.TimeMax=std::max(w.Opt.TimeMax,w.Working.EndTime());w.Dirty=false;w.Error.clear();return true;
    }catch(const std::exception& e){w.Error=std::string("Could not read curve: ")+e.what();return false;}
}
bool Save(Window& w) {
    try {
    if(w.Bound.Source->WriteCurve) {
        if(!w.Bound.Source->WriteCurve(w.Working,w.Error))return false;
        w.SourceCurve=w.Working.ToJson(false).dump();
        w.Dirty=false;w.Error.clear();return true;
    }
    json doc;if(!Document(*w.Bound.Source,doc,w.Error))return false;
    const auto canonical=json::parse(w.Bound.Source->Canonical,nullptr,false);
    if(canonical.is_object()&&doc.value("version",0)!=canonical.value("version",0))doc=canonical;
    json::json_pointer ptr;
    if(!Resolve(doc,w.Bound.Pointer,ptr)){w.Error="This curve was removed or renamed.";return false;}
    doc[ptr]=w.Working.ToJson(false);
    const std::string text=doc.dump(2)+"\n";
    if(w.Bound.Source->Validate&&!w.Bound.Source->Validate(text,w.Error))return false;
    if(!AtomicFile::WriteBytes(fs::u8path(w.Bound.Source->Path),text)){w.Error="Could not save this curve.";return false;}
    if(w.Bound.Source->OnCommit)w.Bound.Source->OnCommit("Edit "+w.Bound.Title);
    std::error_code ec;w.Stamp=fs::last_write_time(fs::u8path(w.Bound.Source->Path),ec);w.Dirty=false;w.Error.clear();return true;
    }catch(const std::exception& e){w.Error=std::string("Could not save curve: ")+e.what();return false;}
}
void OptionsFor(Window& w,const Options& o) {
    w.Opt=o;w.Format=o.ValueFormat?o.ValueFormat:"%.3f";w.Opt.ValueFormat=nullptr;w.Opt.Title=nullptr;
    if(o.Default)w.Default=*o.Default;w.Opt.Default=nullptr;
}
}
AssetScope::AssetScope(const std::string& path,const std::string& canonical,
    std::function<bool(const std::string&,std::string&)> validate,std::function<void(const std::string&)> commit) {
    Previous=activeScope;activeScope=this;ContextLifetime();
    Source=std::make_shared<AssetSource>(AssetSource{path,canonical,std::move(validate),std::move(commit)});
}
AssetScope::~AssetScope(){activeScope=Previous;}
void AssetScope::Bind(const Curve& curve,const std::string& pointer,const std::string& title,const Options& options) {
    Fields[&curve]={pointer,title};auto& b=bindings[{ImGui::GetCurrentContext(),Source->Path+"|"+pointer}];
    b.Source=Source;b.Pointer=pointer;b.Title=title;b.Opt.TimeInSeconds=options.TimeInSeconds;
    if(b.Opt.TimeInSeconds)b.Opt.TimeMax=std::max(b.Opt.TimeMax,curve.EndTime());
}
bool Draw(const char* id,Curve& curve,const ImVec2& size,const Options& o) {
    ContextLifetime();Binding bound;std::string key=std::to_string(ImGui::GetID(id));
    if(activeScope)if(const auto it=activeScope->Fields.find(&curve);it!=activeScope->Fields.end()) {
        key=activeScope->Source->Path+"|"+it->second.Pointer;auto& b=bindings[{ImGui::GetCurrentContext(),key}];
        b.Opt=o;b.Format=o.ValueFormat?o.ValueFormat:"%.3f";b.Opt.ValueFormat=nullptr;b.Opt.Title=nullptr;b.Opt.Default=nullptr;
        b.HasDefault=o.Default!=nullptr;if(o.Default)b.Default=*o.Default;bound=b;
    }
    bool committed=false;auto found=windows.find({ImGui::GetCurrentContext(),key});
    if(found!=windows.end()&&!bound.Source&&found->second.Pending){curve=found->second.Working;found->second.Pending=false;committed=true;}
    const float width=size.x>0?size.x:ImGui::GetContentRegionAvail().x;
    const bool clicked=Preview(id,curve,{std::max(40.0f,width-ImGui::CalcTextSize("Edit Curve").x-30),std::min(size.y,ImGui::GetFontSize()*3.5f)},o);
    ImGui::SameLine();ImGui::PushID(id);const bool button=ImGui::SmallButton("Edit Curve");ImGui::PopID();
    if(clicked||button) {
        auto& w=windows[{ImGui::GetCurrentContext(),key}];
        if(w.Key.empty()){w.Key=key;w.Working=curve;OptionsFor(w,o);}
        w.Bound=bound;if(w.Bound.Title.empty())w.Bound.Title=o.Title?o.Title:"Curve";w.Focus=true;
    }
    return committed;
}
bool DrawBound(const char* id,Curve& curve,const ImVec2& size,const Options& options,
               const std::string& identity,std::function<bool(Curve&,std::string&)> read,
               std::function<bool(const Curve&,std::string&)> write) {
    AssetScope scope(identity,"",{});
    scope.Source->ReadCurve=std::move(read);scope.Source->WriteCurve=std::move(write);
    scope.Bind(curve,"/curve",options.Title?options.Title:"Curve",options);
    return Draw(id,curve,size,options);
}
void DrawWindows(const std::function<void()>& undo,const std::function<void()>& redo,bool canUndo,bool canRedo) {
    auto* ctx=ImGui::GetCurrentContext();
    for(auto it=windows.begin();it!=windows.end();) {
        if(it->first.first!=ctx){++it;continue;}auto& w=it->second;bool open=true;
        const std::string file=w.Bound.Source?fs::u8path(w.Bound.Source->Path).stem().u8string():"Curve";
        const std::string title=file+" / "+w.Bound.Title+"###curve:"+w.Key;
        ImGui::SetNextWindowSize({1100,740},ImGuiCond_FirstUseEver);
        if(w.Focus){ImGui::SetNextWindowFocus();w.Focus=false;}
        if(ImGui::Begin(title.c_str(),&open)) {
            if(w.Bound.Source) {
                if(const auto b=bindings.find({ctx,w.Key});b!=bindings.end())w.Bound.Source=b->second.Source;
                if(w.Bound.Source->ReadCurve && !w.Dirty) {
                    Curve external;
                    if(w.Bound.Source->ReadCurve(external,w.Error)) {
                        const std::string value=external.ToJson(false).dump();
                        if(value!=w.SourceCurve){w.Working=std::move(external);w.SourceCurve=value;}
                        w.Error.clear();
                    }
                }
                else if(!w.Bound.Source->ReadCurve) {
                    std::error_code ec;const auto stamp=fs::last_write_time(fs::u8path(w.Bound.Source->Path),ec);
                    if(!ec&&stamp!=w.Stamp&&!w.Dirty&&Read(w))w.Stamp=stamp;
                }
            }
            ImGui::BeginDisabled(!canUndo);if(ImGui::Button("Undo")&&undo)undo();ImGui::EndDisabled();ImGui::SameLine();
            ImGui::BeginDisabled(!canRedo);if(ImGui::Button("Redo")&&redo)redo();ImGui::EndDisabled();
            if(w.Bound.Source){ImGui::SameLine();if(ImGui::Button("Reload"))Read(w);ImGui::SameLine();ImGui::TextDisabled("%s",fs::u8path(w.Bound.Source->Path).filename().u8string().c_str());}
            if(w.Opt.TimeInSeconds){ImGui::SetNextItemWidth(120);ImGui::DragFloat("Edit end (s)",&w.Opt.TimeMax,.01f,std::max(.01f,w.Working.EndTime()),600,"%.3f",ImGuiSliderFlags_AlwaysClamp);}
            if(!w.Error.empty()){ImGui::TextWrapped("%s",w.Error.c_str());if(w.Dirty&&ImGui::Button("Save Curve"))Save(w);}
            if(w.Bound.Source) {
                ImGui::BeginChild("channels",{180,0},ImGuiChildFlags_Borders);ImGui::TextUnformatted("Curves");
                for(const auto& b:bindings)if(b.first.first==ctx&&b.second.Source->Path==w.Bound.Source->Path) {
                    ImGui::PushID(b.first.second.c_str());
                    if(ImGui::Selectable(b.second.Title.c_str(),b.first.second==w.Key)&&b.first.second!=w.Key) {
                        auto& other=windows[b.first];if(other.Key.empty()){other.Key=b.first.second;other.Bound=b.second;OptionsFor(other,b.second.Opt);other.Format=b.second.Format;other.Default=b.second.Default;Read(other);}other.Focus=true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();ImGui::SameLine();
            }
            ImGui::BeginChild("tools",{0,0});auto options=w.Opt;options.ValueFormat=w.Format.c_str();options.Default=w.Bound.HasDefault?&w.Default:nullptr;
            options.ExternalHistory=bool(undo)||bool(redo);
            const bool committed=DrawCanvas("curve",w.Working,{0,ImGui::GetContentRegionAvail().y},options);ImGui::EndChild();
            if(committed){w.Dirty=true;if(w.Bound.Source)Save(w);else w.Pending=true;}
        }
        ImGui::End();if(!open)it=windows.erase(it);else ++it;
    }
}
}
