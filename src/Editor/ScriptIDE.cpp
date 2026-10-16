#include "EditorUIPrimitives.h"
#include "ScriptIDE.h"
#include "../../extern/ImGuiColorTextEdit/TextEditor.h"
#include "AtomicFile.h"
#include "ProjectPaths.h"
#include "EditorTheme.h"
#include "ScriptIDEThemes.h"
#include "GameModuleBuild.h"
#include <imgui.h>
#include <json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <regex>
#include <set>
#include <sstream>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
namespace fs=std::filesystem;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
std::string Read(const fs::path& path) {std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot read "+path.u8string());return {std::istreambuf_iterator<char>(f),{}};}
std::wstring Wide(const std::string& text) {int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);if(!size && !text.empty())throw std::runtime_error("Invalid UTF-8 source");std::wstring result(size,0);MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size);return result;}
std::string Canonical(const std::string& path) {return fs::absolute(fs::u8path(path)).lexically_normal().u8string();}
bool Same(const std::string& a,const std::string& b) {return _stricmp(a.c_str(),b.c_str())==0;}
bool Field(const char* label,std::string& text,float width=0) {
    if(width>0)ImGui::SetNextItemWidth(width);
    auto resize=[](ImGuiInputTextCallbackData* data)->int {if(data->EventFlag==ImGuiInputTextFlags_CallbackResize){auto& s=*static_cast<std::string*>(data->UserData);s.resize(data->BufTextLen);data->Buf=s.data();}return 0;};
    return ImGui::InputText(label,text.data(),text.capacity()+1,ImGuiInputTextFlags_CallbackResize,resize,&text);
}
// What the IDE edits: the engine's C++ (when this editor was built from a source checkout), the
// project's shaders and its Transform Controller scripts.
enum class Kind { None, Cpp, Shader, Script };
Kind KindOf(const fs::path& path) {
    auto ext=path.extension().u8string();for(auto& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if(ext==".cpp"||ext==".cc"||ext==".c"||ext==".h"||ext==".hpp"||ext==".inl")return Kind::Cpp;
    if(ext==".glsl"||ext==".vert"||ext==".frag"||ext==".comp"||ext==".geom"||ext==".shader")return Kind::Shader;
    if(ext==".tescript")return Kind::Script;
    return Kind::None;
}
TextEditor::LanguageDefinition Language(Kind kind) {
    if(kind==Kind::Cpp)return TextEditor::LanguageDefinition::CPlusPlus();
    if(kind==Kind::Shader)return TextEditor::LanguageDefinition::GLSL();
    auto plain=TextEditor::LanguageDefinition::C();plain.mName="Script";plain.mKeywords.clear();plain.mIdentifiers.clear();
    for(auto word:{"true","false","null"})plain.mKeywords.insert(word);
    return plain;
}
bool Under(const std::string& path,const fs::path& folder) {
    auto root=Canonical(folder.u8string());if(!root.empty() && root.back()!='\\')root+='\\';
    return path.size()>root.size() && _strnicmp(path.c_str(),root.c_str(),root.size())==0;
}
fs::path EngineSources() {return GameModuleBuild::Available()?GameModuleBuild::SourceRoot()/"src":fs::path();}
bool Authored(const std::string& path) {
    if(Under(path,fs::u8path(ProjectPaths::Root())) && !Under(path,fs::u8path(ProjectPaths::Resolve("Library"))))return true;
    const auto engine=EngineSources();return !engine.empty() && Under(path,engine);
}
std::string Label(const std::string& path) {
    const auto engine=EngineSources();
    if(!engine.empty() && Under(path,engine))return "src/"+fs::relative(fs::u8path(path),engine).generic_u8string();
    return fs::u8path(ProjectPaths::Relativize(path)).generic_u8string();
}
// Every editable file: the engine's sources (C++ and built-in shaders) first, then the project's
// shaders and scripts, each sorted.
std::vector<std::string> ScanSources(const std::string& project,const fs::path& engine) {
    std::vector<std::string> paths;std::error_code ec;
    auto scan=[&](const fs::path& root,bool cpp) {
        if(root.empty() || !fs::is_directory(root,ec))return;
        const auto begin=paths.size();
        for(auto it=fs::recursive_directory_iterator(root,fs::directory_options::skip_permission_denied,ec);!ec && it!=fs::recursive_directory_iterator();it.increment(ec)) {
            if(it->is_directory(ec)){const auto name=it->path().filename().u8string();if(name=="Library"||name==".git")it.disable_recursion_pending();continue;}
            const Kind kind=KindOf(it->path());if(kind==Kind::None || (kind==Kind::Cpp && !cpp))continue;
            paths.push_back(it->path().u8string());
        }
        std::sort(paths.begin()+static_cast<std::ptrdiff_t>(begin),paths.end());
    };
    scan(engine,true);scan(fs::u8path(project),false);
    return paths;
}
// MSVC: "C:\path\file.cpp(12,5): error C2065: 'x': undeclared identifier" (column optional).
Json ParseProblems(const std::string& log) {
    Json problems=Json::array();std::set<std::string> seen;
    static const std::regex diagnostic(R"(^\s*(?:\d+>)?(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*(?:fatal\s+)?(error|warning)\s+(\w+)\s*:\s*(.+?)(?:\s+\[.*\])?\s*$)");
    std::istringstream lines(log);std::string line;
    while(std::getline(lines,line)) {
        std::smatch match;if(!std::regex_match(line,match,diagnostic) || !seen.insert(line).second)continue;
        problems.push_back({{"severity",match[4].str()},{"code",match[5].str()},{"message",match[6].str()},
            {"location",{{"path",Canonical(match[1].str())},{"line",std::stoi(match[2].str())},{"column",match[3].matched?std::stoi(match[3].str()):1}}}});
    }
    return problems;
}
}
struct ScriptIDE::Impl {
    struct Document {
        std::string Path,Saved,DiskBytes;TextEditor Editor;bool Bom=false,CRLF=false,Conflict=false;Kind Type=Kind::None;
        bool Dirty() const {return Editor.GetText()!=Saved;}
        std::string Bytes() const {auto text=Editor.GetText();if(CRLF){std::string expanded;for(char c:text){if(c=='\n')expanded+='\r';expanded+=c;}text=std::move(expanded);}return (Bom?"\xEF\xBB\xBF":"")+text;}
    };
    std::vector<std::unique_ptr<Document>> Documents;
    Document* Active=nullptr;bool Visible=false,Focus=false,FindVisible=false,ReplaceVisible=false,KeyboardActive=false;
    int Theme=0;unsigned int SceneDockId=0;bool DockToScene=true;
    float SidebarWidth=235.0f,InformationHeight=200.0f;
    std::string Activate,ClosePath,Status,Find,Replace,ProjectSearch,CompletionFilter,SourceFilter;
    int GoLine=1;bool GoTo=false,CompletionOpen=false,ProjectFind=false;
    Json SearchResults=Json::array(),BuildProblems=Json::array();
    std::vector<std::string> Completions;
    std::string BuildLog;bool WasBuilding=false;
    Clock::time_point LastDiskCheck=Clock::now();
    fs::path SessionPath;
    std::vector<std::string> ProjectFiles;
    std::future<std::vector<std::string>> FileScan;
    Clock::time_point LastScan=Clock::now()-std::chrono::seconds(5);
    Impl() {
        SessionPath=fs::u8path(ScriptIDE::SessionFile());std::error_code ec;fs::create_directories(SessionPath.parent_path(),ec);
        try {if(fs::exists(SessionPath)) {
            auto session=Json::parse(Read(SessionPath));const bool visible=session.value("visible",false);
            Theme=std::clamp(session.value("theme",0),0,static_cast<int>(ScriptIDEThemes::Presets.size())-1);
            SidebarWidth=std::clamp(session.value("sidebarWidth",235.0f),80.0f,2000.0f);
            InformationHeight=std::clamp(session.value("informationHeight",200.0f),64.0f,2000.0f);
            for(const auto& saved:session.value("documents",Json::array())) {
                const auto path=saved.at("path").get<std::string>();if(!fs::is_regular_file(fs::u8path(path)) || KindOf(fs::u8path(path))==Kind::None)continue;
                auto* doc=Open(path);if(!doc)continue;
                if(saved.value("dirty",false)) {doc->Editor.SetText(saved.at("text").get<std::string>());doc->Conflict=saved.value("disk",std::string())!=doc->DiskBytes;Status="Recovered unsaved buffers.";}
            }
            const auto active=session.value("active",std::string());if(!active.empty() && fs::is_regular_file(fs::u8path(active)))Open(active);
            Visible=visible;Focus=visible;
        }} catch(const std::exception& e){Status=e.what();}
    }
    ~Impl() {Persist();}
    void Persist() {
        Json documents=Json::array();for(const auto& d:Documents)documents.push_back({{"path",d->Path},{"dirty",d->Dirty()},{"text",d->Editor.GetText()},{"disk",d->DiskBytes}});
        AtomicFile::WriteBytes(SessionPath,Json{{"visible",Visible},{"theme",Theme},{"sidebarWidth",SidebarWidth},{"informationHeight",InformationHeight},{"active",Active?Active->Path:""},{"documents",documents}}.dump());
    }
    Document* Open(const std::string& path,int line=0,int column=1) {
        try {
            if(path.empty()){Visible=Focus=true;return Active;}
            auto absolute=Canonical(path);for(auto& d:Documents)if(Same(d->Path,absolute)){Active=d.get();Activate=d->Path;Visible=Focus=true;Jump(*d,line,column);return d.get();}
            const Kind kind=KindOf(fs::u8path(absolute));
            if(kind==Kind::None)throw std::runtime_error("The Script IDE opens C++ sources, shaders and .tescript files.");
            auto bytes=Read(fs::u8path(absolute));if(bytes.size()>8*1024*1024)throw std::runtime_error("Source file exceeds the 8 MB editor limit.");
            if(bytes.find('\0')!=std::string::npos)throw std::runtime_error("Use UTF-8 source; binary/UTF-16 files cannot be edited here.");
            auto doc=std::make_unique<Document>();doc->Path=absolute;doc->Type=kind;doc->DiskBytes=bytes;doc->CRLF=bytes.find("\r\n")!=std::string::npos;
            doc->Bom=bytes.compare(0,3,"\xEF\xBB\xBF")==0;if(doc->Bom)bytes.erase(0,3);Wide(bytes);
            doc->Editor.SetLanguageDefinition(Language(kind));doc->Editor.SetShowWhitespaces(false);doc->Editor.SetTabSize(4);doc->Editor.SetText(bytes);doc->Saved=doc->Editor.GetText();doc->Editor.SetReadOnly(!Authored(absolute));
            Active=doc.get();Jump(*doc,line,column);Documents.push_back(std::move(doc));Activate=absolute;Visible=Focus=true;MarkProblems();Persist();return Active;
        }catch(const std::exception& e){Status=e.what();return nullptr;}
    }
    static void Jump(Document& doc,int line,int column) {
        if(line<=0)return;auto lines=doc.Editor.GetTextLines();if(lines.empty())return;int row=std::clamp(line-1,0,static_cast<int>(lines.size())-1);
        const auto& s=lines[row];int chars=0,visual=0;
        for(size_t i=0;i<s.size() && chars<column-1;) {unsigned char c=s[i];size_t n=c<128?1:c<224?2:c<240?3:4;if(c=='\t')visual=(visual/4+1)*4;else ++visual;++chars;i+=n;}
        doc.Editor.SetCursorPosition({row,visual});doc.Editor.SetSelection({row,visual},{row,visual});
    }
    // Compiler errors and warnings of the last module build, as editor line markers.
    void MarkProblems() {
        for(auto& d:Documents) {
            TextEditor::ErrorMarkers markers;
            for(const auto& p:BuildProblems)if(Same(p["location"]["path"].get<std::string>(),d->Path))
                markers[p["location"]["line"].get<int>()]+=p["code"].get<std::string>()+": "+p["message"].get<std::string>()+"\n";
            d->Editor.SetErrorMarkers(markers);
        }
    }
    void AfterSave(const Document& doc) {
        const auto file=fs::u8path(doc.Path);
        if(doc.Type==Kind::Cpp) {
            if(GameModuleBuild::IsReloadableSource(file)){GameModuleBuild::Request();Status="Saved "+file.filename().u8string()+"; rebuilding the modules for hot reload.";}
            else Status="Saved "+file.filename().u8string()+". It is part of the editor executable: rebuild and restart to use it.";
        } else if(doc.Type==Kind::Shader)Status="Saved "+file.filename().u8string()+"; shaders reload on their own.";
        else Status="Saved "+file.filename().u8string()+".";
    }
    bool Save(Document& doc,bool overwrite=false) {
        if(doc.Editor.IsReadOnly()){Status="This file is read-only here.";return false;}
        try {
            std::error_code ec;const bool exists=fs::is_regular_file(fs::u8path(doc.Path),ec);
            if(!overwrite && ((!exists && !doc.DiskBytes.empty()) || (exists && Read(fs::u8path(doc.Path))!=doc.DiskBytes))){doc.Conflict=true;Status="File changed outside the IDE. Reload or explicitly overwrite before saving.";return false;}
            auto bytes=doc.Bytes();if(!AtomicFile::WriteBytes(fs::u8path(doc.Path),bytes,true)){Status="Save failed: "+doc.Path;return false;}
            doc.DiskBytes=bytes;doc.Saved=doc.Editor.GetText();doc.Conflict=false;AfterSave(doc);Persist();return true;
        }catch(const std::exception& e){Status=e.what();return false;}
    }
    bool SaveAll() {bool okay=true;for(auto& d:Documents)if(d->Dirty())okay=Save(*d)&&okay;return okay;}
    void Close(const std::string& path) {
        auto found=std::find_if(Documents.begin(),Documents.end(),[&](const auto& d){return Same(d->Path,path);});
        if(found==Documents.end())return;
        if(Active==found->get())Active=nullptr;Documents.erase(found);
        if(!Active && !Documents.empty()){Active=Documents.front().get();Activate=Active->Path;}
        Persist();
    }
    void Reload(Document& doc) {
        try {
            auto bytes=Read(fs::u8path(doc.Path));
            if(bytes.size()>8*1024*1024 || bytes.find('\0')!=std::string::npos)throw std::runtime_error("Disk source must be UTF-8 and no larger than 8 MB.");
            auto disk=bytes;bool bom=bytes.compare(0,3,"\xEF\xBB\xBF")==0;if(bom)bytes.erase(0,3);Wide(bytes);
            doc.DiskBytes=std::move(disk);doc.Bom=bom;doc.CRLF=bytes.find("\r\n")!=std::string::npos;doc.Editor.SetText(bytes);doc.Saved=doc.Editor.GetText();doc.Conflict=false;Persist();
        }catch(const std::exception& e){doc.Conflict=true;Status=e.what();}
    }
    void Poll() {
        if(FileScan.valid() && FileScan.wait_for(std::chrono::milliseconds(0))==std::future_status::ready)try {ProjectFiles=FileScan.get();}catch(const std::exception& e){Status=e.what();}
        if(!FileScan.valid() && Clock::now()-LastScan>std::chrono::seconds(3)) {
            LastScan=Clock::now();FileScan=std::async(std::launch::async,ScanSources,ProjectPaths::Root(),EngineSources());
        }
        const bool building=GameModuleBuild::Running();
        if(WasBuilding && !building){BuildLog=GameModuleBuild::LastLog();BuildProblems=ParseProblems(BuildLog);MarkProblems();}
        WasBuilding=building;
        if(Clock::now()-LastDiskCheck>std::chrono::seconds(1)) {
            LastDiskCheck=Clock::now();for(auto& d:Documents)try {
                auto disk=Read(fs::u8path(d->Path));if(disk!=d->DiskBytes){if(d->Dirty())d->Conflict=true;else Reload(*d);}
            }catch(...){d->Conflict=true;}
        }
    }
    // Ctrl+Space: the identifiers in the open buffers that start with the word at the caret.
    void Complete() {
        Completions.clear();if(!Active)return;
        auto line=Active->Editor.GetCurrentLineText();int byte=Active->Editor.CursorByteIndex(),start=byte;
        while(start>0 && (std::isalnum(static_cast<unsigned char>(line[start-1])) || line[start-1]=='_'))--start;
        const std::string prefix=line.substr(start,byte-start);
        std::set<std::string> words;
        for(const auto& d:Documents){const auto text=d->Editor.GetText();std::string word;
            for(size_t i=0;i<=text.size();++i){const char c=i<text.size()?text[i]:' ';
                if(std::isalnum(static_cast<unsigned char>(c))||c=='_')word+=c;
                else{if(word.size()>2 && !std::isdigit(static_cast<unsigned char>(word[0])) && word!=prefix && word.rfind(prefix,0)==0)words.insert(word);word.clear();}}}
        Completions.assign(words.begin(),words.end());if(Completions.size()>400)Completions.resize(400);
        CompletionOpen=!Completions.empty();if(!CompletionOpen)Status="No completions for '"+prefix+"'.";
    }
    void ApplyCompletion(const std::string& name) {
        if(!Active)return;auto pos=Active->Editor.GetCursorPosition();auto line=Active->Editor.GetCurrentLineText();int byte=Active->Editor.CursorByteIndex(),start=byte;
        while(start>0 && (std::isalnum(static_cast<unsigned char>(line[start-1])) || line[start-1]=='_'))--start;
        auto begin=pos;begin.mColumn-=byte-start;Active->Editor.SetSelection(begin,pos);Active->Editor.ReplaceSelection(name);CompletionOpen=false;Persist();
    }
    void FindNext(bool reverse=false) {
        if(!Active || Find.empty())return;auto text=Active->Editor.GetText();auto lines=Active->Editor.GetTextLines();auto c=Active->Editor.GetCursorPosition();size_t offset=Active->Editor.CursorByteIndex();for(int i=0;i<c.mLine;++i)offset+=lines[i].size()+1;
        size_t found=reverse?text.rfind(Find,offset>0?offset-1:0):text.find(Find,offset);if(found==std::string::npos)found=reverse?text.rfind(Find):text.find(Find);
        if(found==std::string::npos){Status="No match.";return;}
        int row=static_cast<int>(std::count(text.begin(),text.begin()+found,'\n'));auto last=text.rfind('\n',found);size_t byte=found-(last==std::string::npos?0:last+1);
        int col=static_cast<int>(Wide(lines[row].substr(0,byte)).size())+1;Jump(*Active,row+1,col);auto begin=Active->Editor.GetCursorPosition();Jump(*Active,row+1,col+static_cast<int>(Wide(Find).size()));Active->Editor.SetSelection(begin,Active->Editor.GetCursorPosition());
    }
    void SearchProject() {
        SearchResults=Json::array();if(ProjectSearch.empty())return;
        for(const auto& path:ProjectFiles) {
            if(SearchResults.size()>=500)break;
            try {
                std::string text;for(const auto& d:Documents)if(Same(d->Path,path)){text=d->Editor.GetText();break;}if(text.empty())text=Read(fs::u8path(path));
                size_t offset=0;while((offset=text.find(ProjectSearch,offset))!=std::string::npos && SearchResults.size()<500){int row=static_cast<int>(std::count(text.begin(),text.begin()+offset,'\n'))+1;auto start=text.rfind('\n',offset);start=start==std::string::npos?0:start+1;SearchResults.push_back({{"path",path},{"line",row},{"column",static_cast<int>(Wide(text.substr(start,offset-start)).size())+1}});offset+=ProjectSearch.size();}
            }catch(const std::exception&){}
        }
    }
    void Locations(const Json& places) {int id=0;for(const auto& place:places){ImGui::PushID(++id);auto path=place.at("path").get<std::string>();auto label=fs::u8path(path).filename().u8string()+":"+std::to_string(place["line"].get<int>());if(ImGui::Selectable(label.c_str()))Open(path,place["line"],place["column"]);ImGui::PopID();}}
    void Build() {if(!SaveAll())return;if(!GameModuleBuild::Available()){Status="Module rebuilds need the engine's source checkout and CMake build folder.";return;}GameModuleBuild::Request();}
    void Draw();
};
namespace {std::string g_SessionDirectory;}
void ScriptIDE::SetSessionDirectory(const std::string& dir){g_SessionDirectory=dir;}
std::string ScriptIDE::SessionFile(){return (fs::u8path(g_SessionDirectory.empty()?ProjectPaths::Resolve("Library/IDE"):g_SessionDirectory)/"session.json").u8string();}
ScriptIDE::ScriptIDE():impl(std::make_unique<Impl>()){}
ScriptIDE::~ScriptIDE()=default;
void ScriptIDE::Open(const std::string& path,int line,int column){impl->Open(path,line,column);}
void ScriptIDE::Show(){impl->Visible=impl->Focus=true;}
void ScriptIDE::Draw(unsigned int sceneDockId){impl->SceneDockId=sceneDockId;impl->Draw();}
bool ScriptIDE::HasUnsavedFiles() const {return std::any_of(impl->Documents.begin(),impl->Documents.end(),[](const auto& d){return d->Dirty();});}
bool ScriptIDE::OwnsKeyboard() const {return impl->KeyboardActive;}
bool ScriptIDE::SaveAll(){return impl->SaveAll();}
void ScriptIDE::CancelPrompts(){impl->ClosePath.clear();}
void ScriptIDE::DiscardAll(){for(auto& d:impl->Documents)if(d->Dirty()){impl->Reload(*d);if(d->Dirty())d->Editor.SetText(d->Saved);}impl->Persist();}

void ScriptIDE::Impl::Draw() {
    Poll();KeyboardActive=false;if(!Visible)return;
    const auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x+viewport->WorkSize.x*.5f,viewport->WorkPos.y+viewport->WorkSize.y*.5f),ImGuiCond_FirstUseEver,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(1100,740),ImGuiCond_FirstUseEver);if(Focus){ImGui::SetNextWindowFocus();Focus=false;}
    if(DockToScene && SceneDockId){ImGui::SetNextWindowDockID(SceneDockId,ImGuiCond_Always);DockToScene=false;}
    if(!ImGui::Begin("Script IDE",&Visible,ImGuiWindowFlags_MenuBar)){ImGui::End();return;}
    const bool focused=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    KeyboardActive=focused;
    auto& io=ImGui::GetIO();bool saveKey=focused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S,false);
    if(ImGui::BeginMenuBar()) {
        if(ImGui::BeginMenu("File")) {if(ImGui::MenuItem("Save","Ctrl+S",false,Active!=nullptr))Save(*Active);if(ImGui::MenuItem("Save All","Ctrl+Shift+S"))SaveAll();if(ImGui::MenuItem("Reload from disk",nullptr,false,Active!=nullptr))ClosePath="reload:"+Active->Path;ImGui::EndMenu();}
        if(ImGui::BeginMenu("Edit")) {
            if(ImGui::MenuItem("Undo","Ctrl+Z",false,Active && Active->Editor.CanUndo())){Active->Editor.Undo();Persist();}
            if(ImGui::MenuItem("Redo","Ctrl+Y",false,Active && Active->Editor.CanRedo())){Active->Editor.Redo();Persist();}
            if(ImGui::MenuItem("Find / Replace","Ctrl+F"))FindVisible=true;
            if(ImGui::MenuItem("Complete word","Ctrl+Space",false,Active && !Active->Editor.IsReadOnly()))Complete();
            ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("Navigate")) {
            if(ImGui::MenuItem("Go to line","Ctrl+G"))GoTo=true;
            if(ImGui::MenuItem("Find in project","Ctrl+Shift+F"))ProjectFind=true;ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("Build")) {
            if(ImGui::MenuItem("Save All & Rebuild Modules","F6",false,GameModuleBuild::Available()))Build();
            if(!GameModuleBuild::Available() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("Needs the engine's source checkout and CMake build folder.");
            ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("Appearance")) {
            ImGui::TextDisabled("Font: JetBrains Mono");ImGui::Separator();
            for(int i=0;i<static_cast<int>(ScriptIDEThemes::Presets.size());++i)
                if(ImGui::MenuItem(ScriptIDEThemes::Presets[i].Name,nullptr,Theme==i)){Theme=i;Persist();}
            ImGui::Separator();if(ImGui::MenuItem("Dock alongside Scene / Game"))DockToScene=true;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    if(saveKey){if(io.KeyShift)SaveAll();else if(Active)Save(*Active);}
    if(focused) {
        if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F,false)){if(io.KeyShift)ProjectFind=true;else FindVisible=true;}
        if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G,false))GoTo=true;
        if(ImGui::IsKeyPressed(ImGuiKey_F6,false))Build();
        if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space,false))Complete();
    }
    const float scale=EditorTheme::Scale(),divider=6.0f*scale;
    const auto space=ImGui::GetContentRegionAvail();
    const float sidebarMin=std::min(80.0f*scale,std::max(1.0f,space.x*.25f));
    const float sidebarMax=std::max(sidebarMin,space.x-divider-std::min(180.0f*scale,space.x*.5f));
    const float sidebar=std::clamp(SidebarWidth*scale,sidebarMin,sidebarMax);
    ImGui::BeginChild("Sources",ImVec2(sidebar,0),ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted("SOURCES");
    Field("##ScriptSearch",SourceFilter,sidebar-20);
    try {for(const auto& path:ProjectFiles) {
        auto label=Label(path);
        if(!SourceFilter.empty() && label.find(SourceFilter)==std::string::npos)continue;
        if(ImGui::Selectable(label.c_str(),Active && Same(Active->Path,path)))Open(path);
    }}catch(const std::exception& e){ImGui::TextWrapped("%s",e.what());}
    ImGui::EndChild();ImGui::SameLine(0,0);
    ImGui::InvisibleButton("##SourcesDivider",ImVec2(divider,std::max(1.0f,space.y)));
    if(ImGui::IsItemHovered() || ImGui::IsItemActive())ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if(ImGui::IsItemActive())SidebarWidth=std::clamp(sidebar+io.MouseDelta.x,sidebarMin,sidebarMax)/scale;
    if(ImGui::IsItemDeactivated())Persist();
    auto dividerMin=ImGui::GetItemRectMin(),dividerMax=ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(dividerMin.x+divider*.5f,dividerMin.y),ImVec2(dividerMin.x+divider*.5f,dividerMax.y),ImGui::GetColorU32(ImGui::IsItemActive()?ImGuiCol_SeparatorActive:ImGui::IsItemHovered()?ImGuiCol_SeparatorHovered:ImGuiCol_Separator));
    ImGui::SameLine(0,0);ImGui::BeginChild("EditorArea",ImVec2(0,0));
    if(Active && Active->Conflict) {
        ImGui::TextColored(ImVec4(1,.7f,.2f,1),"File changed on disk. Your buffer has been kept.");
        if(EditorUIPrimitives::SecondaryButton("Reload disk version"))ClosePath="reload:"+Active->Path;ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Overwrite disk with buffer"))Save(*Active,true);
    }
    if(FindVisible) {
        // A narrow pane wraps the bar instead of clipping its buttons (Close find included).
        const float right=ImGui::GetCursorScreenPos().x+ImGui::GetContentRegionAvail().x;
        auto flow=[&](float width){if(ImGui::GetItemRectMax().x+ImGui::GetStyle().ItemSpacing.x+width<=right)ImGui::SameLine();};
        auto button=[](const char* label){return ImGui::CalcTextSize(label,nullptr,true).x+ImGui::GetStyle().FramePadding.x*2;};
        Field("Find",Find,180);flow(button("Previous"));if(EditorUIPrimitives::SecondaryButton("Previous"))FindNext(true);flow(button("Next"));if(EditorUIPrimitives::SecondaryButton("Next"))FindNext();flow(ImGui::GetFrameHeight()+ImGui::GetStyle().ItemInnerSpacing.x+ImGui::CalcTextSize("Replace").x);ImGui::Checkbox("Replace",&ReplaceVisible);flow(button("Close find"));if(EditorUIPrimitives::SecondaryButton("Close find"))FindVisible=false;
        if(ReplaceVisible && Active && !Active->Editor.IsReadOnly()){Field("With",Replace,180);flow(button("Replace match"));if(EditorUIPrimitives::SecondaryButton("Replace match") && Active->Editor.GetSelectedText()==Find){Active->Editor.ReplaceSelection(Replace);Persist();}flow(button("Replace all"));if(EditorUIPrimitives::SecondaryButton("Replace all") && !Find.empty()){auto text=Active->Editor.GetText();size_t p=0;while((p=text.find(Find,p))!=std::string::npos){text.replace(p,Find.size(),Replace);p+=Replace.size();}Active->Editor.SelectAll();Active->Editor.ReplaceSelection(text);Persist();}}
    }
    const float paneHeight=std::max(1.0f,ImGui::GetContentRegionAvail().y);
    const float infoMin=std::min(64.0f*scale,paneHeight*.3f);
    const float infoMax=std::max(infoMin,paneHeight-divider-std::min(100.0f*scale,paneHeight*.5f));
    const float infoHeight=std::clamp(InformationHeight*scale,infoMin,infoMax);
    ImGui::BeginChild("CodePane",ImVec2(0,std::max(1.0f,paneHeight-infoHeight-divider)),ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    if(ImGui::BeginTabBar("ScriptTabs",ImGuiTabBarFlags_Reorderable|ImGuiTabBarFlags_AutoSelectNewTabs|ImGuiTabBarFlags_TabListPopupButton)) {
        for(auto& holder:Documents) {
            auto& d=*holder;bool open=true;auto title=fs::u8path(d.Path).filename().u8string()+"###"+d.Path;
            ImGuiTabItemFlags flags=d.Dirty()?ImGuiTabItemFlags_UnsavedDocument:ImGuiTabItemFlags_None;if(Same(d.Path,Activate))flags|=ImGuiTabItemFlags_SetSelected;
            if(ImGui::BeginTabItem(title.c_str(),&open,flags)) {
                Active=&d;if(Same(Activate,d.Path))Activate.clear();
                auto cursor=d.Editor.GetCursorPosition();ImGui::TextDisabled("%s  |  Ln %d, Col %d%s",Label(d.Path).c_str(),cursor.mLine+1,cursor.mColumn+1,d.Editor.IsReadOnly()?"  |  read-only":"");
                const auto before=d.Editor.GetText();
                d.Editor.SetPalette(ScriptIDEThemes::Palette(Theme));
                EditorTheme::PushMono();d.Editor.Render("Code",ImVec2(0,std::max(1.0f,ImGui::GetContentRegionAvail().y)));EditorTheme::PopFont();
                if(d.Editor.GetText()!=before){CompletionOpen=false;Persist();}
                ImGui::EndTabItem();
            }
            if(!open)ClosePath=d.Path;
        }
        ImGui::EndTabBar();
    }
    if(!Active)ImGui::TextWrapped("Choose a source on the left, or double-click a shader or .tescript asset. F6 saves and rebuilds the game and editor modules, which hot reload into this editor.");
    ImGui::EndChild();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY()-ImGui::GetStyle().ItemSpacing.y);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(ImGui::GetStyle().ItemSpacing.x,0));
    ImGui::InvisibleButton("##InformationDivider",ImVec2(std::max(1.0f,ImGui::GetContentRegionAvail().x),divider));
    if(ImGui::IsItemHovered() || ImGui::IsItemActive())ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if(ImGui::IsItemActive())InformationHeight=std::clamp(infoHeight-io.MouseDelta.y,infoMin,infoMax)/scale;
    if(ImGui::IsItemDeactivated())Persist();
    dividerMin=ImGui::GetItemRectMin();dividerMax=ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(dividerMin.x,dividerMin.y+divider*.5f),ImVec2(dividerMax.x,dividerMin.y+divider*.5f),ImGui::GetColorU32(ImGui::IsItemActive()?ImGuiCol_SeparatorActive:ImGui::IsItemHovered()?ImGuiCol_SeparatorHovered:ImGuiCol_Separator));
    ImGui::BeginChild("InformationPane",ImVec2(0,0));
    ImGui::PopStyleVar();
    const bool compileErrors=std::any_of(BuildProblems.begin(),BuildProblems.end(),[](const Json& d){return d["severity"]=="error";});
    ImGui::TextDisabled("%s",GameModuleBuild::Running()?"Rebuilding modules...":!GameModuleBuild::Available()?"Module rebuilds unavailable (no source checkout)":
                             compileErrors || !GameModuleBuild::LastSucceeded()?"Build failed - check Problems":"Modules ready");
    if(!Status.empty())ImGui::TextWrapped("%s",Status.c_str());
    if(ImGui::BeginTabBar("IDEInformation")) {
        if(ImGui::BeginTabItem("Problems")) {
            ImGui::BeginChild("Diagnostics",ImVec2(0,0));int id=0;
            ImGui::TextDisabled("LAST MODULE BUILD");
            for(const auto& problem:BuildProblems){ImGui::PushID(++id);auto label=problem["severity"].get<std::string>()+" "+problem["code"].get<std::string>()+": "+problem["message"].get<std::string>();if(ImGui::Selectable(label.c_str())){const auto& l=problem["location"];Open(l["path"],l["line"],l["column"]);}ImGui::PopID();}
            ImGui::EndChild();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Build output")){ImGui::BeginChild("BuildLog",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);ImGui::TextUnformatted(BuildLog.c_str());ImGui::EndChild();ImGui::EndTabItem();}
        ImGui::EndTabBar();
    }
    ImGui::EndChild(); // InformationPane
    ImGui::EndChild();
    if(CompletionOpen){ImGui::OpenPopup("Completion");CompletionOpen=false;}
    if(ImGui::BeginPopup("Completion")) {
        Field("Filter",CompletionFilter,220);ImGui::BeginChild("Candidates",ImVec2(470,220));int id=0;
        for(const auto& name:Completions){if(!CompletionFilter.empty() && name.find(CompletionFilter)==std::string::npos)continue;ImGui::PushID(++id);if(ImGui::Selectable(name.c_str())){ApplyCompletion(name);CompletionFilter.clear();ImGui::CloseCurrentPopup();}ImGui::PopID();}ImGui::EndChild();ImGui::EndPopup();
    }
    if(GoTo){ImGui::OpenPopup("Go to line");GoTo=false;}
    if(ImGui::BeginPopup("Go to line")){ImGui::InputInt("Line",&GoLine);if(EditorUIPrimitives::SecondaryButton("Go") && Active){Jump(*Active,GoLine,1);ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
    if(ProjectFind){ImGui::OpenPopup("Find in project");ProjectFind=false;}
    if(ImGui::BeginPopup("Find in project")){Field("Text",ProjectSearch,300);ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Search"))SearchProject();ImGui::BeginChild("ProjectMatches",ImVec2(560,280));Locations(SearchResults);ImGui::EndChild();ImGui::EndPopup();}
    if(!ClosePath.empty()) {
        bool reload=ClosePath.rfind("reload:",0)==0;auto path=reload?ClosePath.substr(7):ClosePath;Document* closing=nullptr;for(auto& d:Documents)if(Same(d->Path,path))closing=d.get();
        if(closing && closing->Dirty())ImGui::OpenPopup("Unsaved script");else {if(closing){if(reload)Reload(*closing);else Close(path);}ClosePath.clear();}
    }
    if(ImGui::BeginPopupModal("Unsaved script",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Save changes before closing or reloading this file?");
        if(EditorUIPrimitives::SecondaryButton("Save")){auto path=ClosePath.rfind("reload:",0)==0?ClosePath.substr(7):ClosePath;for(auto& d:Documents)if(Same(d->Path,path) && Save(*d)){if(ClosePath.rfind("reload:",0)!=0)Close(path);ClosePath.clear();ImGui::CloseCurrentPopup();break;}}
        ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Discard")){if(ClosePath.rfind("reload:",0)==0){auto path=ClosePath.substr(7);for(auto& d:Documents)if(Same(d->Path,path))Reload(*d);}else Close(ClosePath);ClosePath.clear();ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Cancel")){ClosePath.clear();ImGui::CloseCurrentPopup();}ImGui::EndPopup();
    }
    ImGui::End();
}
