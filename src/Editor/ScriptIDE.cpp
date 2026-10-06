#include "EditorUIPrimitives.h"
#include "ScriptIDE.h"
#include "../../extern/ImGuiColorTextEdit/TextEditor.h"
#include "AtomicFile.h"
#include "EnginePaths.h"
#include "ProjectPaths.h"
#include "EditorTheme.h"
#include "ScriptIDEThemes.h"
#include "Log.h"
#include "Scripting/ScriptRuntime.h"
#include <imgui.h>
#include <json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
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
std::string Utf8(const std::wstring& text) {int size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);std::string result(size,0);WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size,nullptr,nullptr);return result;}
std::string Canonical(const std::string& path) {return fs::absolute(fs::u8path(path)).lexically_normal().u8string();}
bool Same(const std::string& a,const std::string& b) {return _stricmp(a.c_str(),b.c_str())==0;}
bool Authored(const std::string& path) {auto root=Canonical(ProjectPaths::Resolve("assets"));root+='\\';return path.size()>root.size() && _strnicmp(path.c_str(),root.c_str(),root.size())==0;}
bool Field(const char* label,std::string& text,float width=0) {
    if(width>0)ImGui::SetNextItemWidth(width);
    auto resize=[](ImGuiInputTextCallbackData* data)->int {if(data->EventFlag==ImGuiInputTextFlags_CallbackResize){auto& s=*static_cast<std::string*>(data->UserData);s.resize(data->BufTextLen);data->Buf=s.data();}return 0;};
    return ImGui::InputText(label,text.data(),text.capacity()+1,ImGuiInputTextFlags_CallbackResize,resize,&text);
}
TextEditor::LanguageDefinition CSharp() {
    auto language=TextEditor::LanguageDefinition::CPlusPlus();language.mName="C#";
    language.mIdentifiers.clear();language.mPreprocIdentifiers.clear();
    language.mKeywords.clear();
    for(auto word:{"abstract","as","async","await","base","bool","break","byte","case","catch","char","checked","class","const","continue","decimal","default","delegate","do","double","else","enum","event","explicit","extern","false","finally","fixed","float","for","foreach","global","goto","if","implicit","in","int","interface","internal","is","lock","long","namespace","new","null","object","operator","out","override","params","partial","private","protected","public","readonly","record","ref","required","return","sbyte","sealed","short","sizeof","stackalloc","static","string","struct","switch","this","throw","true","try","typeof","uint","ulong","unchecked","unsafe","ushort","using","var","virtual","void","volatile","when","where","while","with","yield"})language.mKeywords.insert(word);
    return language;
}
}
struct ScriptIDE::Impl {
    struct Document {
        std::string Path,Saved,DiskBytes;TextEditor Editor;bool Bom=false,CRLF=false,Conflict=false;
        bool Dirty() const {return Editor.GetText()!=Saved;}
        std::string Bytes() const {auto text=Editor.GetText();if(CRLF){std::string expanded;for(char c:text){if(c=='\n')expanded+='\r';expanded+=c;}text=std::move(expanded);}return (Bom?"\xEF\xBB\xBF":"")+text;}
    };
    std::vector<std::unique_ptr<Document>> Documents;
    Document* Active=nullptr;bool Visible=false,Focus=false,FindVisible=false,ReplaceVisible=false,KeyboardActive=false;
    int Theme=0;unsigned int SceneDockId=0;bool DockToScene=true;
    float SidebarWidth=235.0f,InformationHeight=200.0f;
    std::string Activate,ClosePath,Status,Find,Replace,NewName="NewName",ProjectSearch,CompletionFilter,SourceFilter;
    int GoLine=1,CompletionIndex=0;bool GoTo=false,CompletionOpen=false,RenamePrompt=false,RenamePreview=false,ProjectFind=false;
    Json Result=Json::object(),SearchResults=Json::array(),BuildProblems=Json::array();
    std::map<std::string,std::string> BuildLogs;
    Clock::time_point Changed=Clock::now(),LastDiskCheck=Clock::now();
    bool NeedAnalysis=false;std::string PendingMode="analyze",RunningMode,RunningPath,RunningText;
    int RunningLine=0,RunningColumn=0;TextEditor::Coordinates RunningCursor;HANDLE Process=nullptr;
    fs::path RequestPath,ResponsePath,SessionPath;
    std::string AnalysisError;
    std::vector<std::string> ProjectFiles;
    std::future<std::vector<std::string>> FileScan;
    Clock::time_point LastScan=Clock::now()-std::chrono::seconds(5),HoverStarted=Clock::now();
    TextEditor::Coordinates HoverPosition;
    bool HoverActive=false,HoverInfo=false;
    Impl() {
        const auto cache=fs::u8path(ProjectPaths::Resolve("Library/IDE"));fs::create_directories(cache);
        const auto id=std::to_string(GetCurrentProcessId());RequestPath=cache/(id+"-request.json");ResponsePath=cache/(id+"-response.json");SessionPath=cache/"session.json";
        try {if(fs::exists(SessionPath)) {
            auto session=Json::parse(Read(SessionPath));const bool visible=session.value("visible",false);
            Theme=std::clamp(session.value("theme",0),0,static_cast<int>(ScriptIDEThemes::Presets.size())-1);
            SidebarWidth=std::clamp(session.value("sidebarWidth",235.0f),80.0f,2000.0f);
            InformationHeight=std::clamp(session.value("informationHeight",200.0f),64.0f,2000.0f);
            for(const auto& saved:session.value("documents",Json::array())) {
                const auto path=saved.at("path").get<std::string>();if(!fs::is_regular_file(fs::u8path(path)))continue;
                auto* doc=Open(path);if(!doc)continue;
                if(saved.value("dirty",false)) {doc->Editor.SetText(saved.at("text").get<std::string>());doc->Conflict=saved.value("disk",std::string())!=doc->DiskBytes;Status="Recovered unsaved script buffers.";}
            }
            const auto active=session.value("active",std::string());if(!active.empty())Open(active);
            Visible=visible;Focus=visible;
        }} catch(const std::exception& e){Status=e.what();}
    }
    ~Impl() {Persist();if(Process){TerminateProcess(Process,0);CloseHandle(Process);}std::error_code ec;fs::remove(RequestPath,ec);fs::remove(ResponsePath,ec);}
    void Persist() {
        Json documents=Json::array();for(const auto& d:Documents)documents.push_back({{"path",d->Path},{"dirty",d->Dirty()},{"text",d->Editor.GetText()},{"disk",d->DiskBytes}});
        AtomicFile::WriteBytes(SessionPath,Json{{"visible",Visible},{"theme",Theme},{"sidebarWidth",SidebarWidth},{"informationHeight",InformationHeight},{"active",Active?Active->Path:""},{"documents",documents}}.dump());
    }
    Document* Open(const std::string& path,int line=0,int column=1) {
        try {
            auto absolute=Canonical(path);for(auto& d:Documents)if(Same(d->Path,absolute)){if(Active!=d.get()){Queue();Changed-=std::chrono::milliseconds(450);}Active=d.get();Activate=d->Path;Visible=Focus=true;Jump(*d,line,column);return d.get();}
            if(fs::u8path(absolute).extension()!=".cs")throw std::runtime_error("The Script IDE opens C# source files.");
            auto bytes=Read(fs::u8path(absolute));if(bytes.size()>8*1024*1024)throw std::runtime_error("Source file exceeds the 8 MB editor limit.");
            if(bytes.find('\0')!=std::string::npos)throw std::runtime_error("Use UTF-8 C# source; binary/UTF-16 files cannot be edited here.");
            auto doc=std::make_unique<Document>();doc->Path=absolute;doc->DiskBytes=bytes;doc->CRLF=bytes.find("\r\n")!=std::string::npos;
            doc->Bom=bytes.compare(0,3,"\xEF\xBB\xBF")==0;if(doc->Bom)bytes.erase(0,3);Wide(bytes);
            doc->Editor.SetLanguageDefinition(CSharp());doc->Editor.SetShowWhitespaces(false);doc->Editor.SetTabSize(4);doc->Editor.SetText(bytes);doc->Saved=doc->Editor.GetText();doc->Editor.SetReadOnly(!Authored(absolute));
            Active=doc.get();Jump(*doc,line,column);Documents.push_back(std::move(doc));Activate=absolute;Visible=Focus=true;Queue();Changed-=std::chrono::milliseconds(450);Persist();return Active;
        }catch(const std::exception& e){Status=e.what();return nullptr;}
    }
    static void Jump(Document& doc,int line,int column) {
        if(line<=0)return;auto lines=doc.Editor.GetTextLines();int row=std::clamp(line-1,0,static_cast<int>(lines.size())-1);
        const auto& s=lines[row];int utf16=0,visual=0;
        for(size_t i=0;i<s.size() && utf16<column-1;) {unsigned char c=s[i];size_t n=c<128?1:c<224?2:c<240?3:4;if(c=='\t')visual=(visual/4+1)*4;else ++visual;utf16+=n==4?2:1;i+=n;}
        doc.Editor.SetCursorPosition({row,visual});doc.Editor.SetSelection({row,visual},{row,visual});
    }
    void Queue(std::string mode="analyze") {NeedAnalysis=true;PendingMode=std::move(mode);Changed=Clock::now();}
    bool Save(Document& doc,bool overwrite=false) {
        if(doc.Editor.IsReadOnly()){Status="Referenced source is read-only.";return false;}
        try {
            std::error_code ec;const bool exists=fs::is_regular_file(fs::u8path(doc.Path),ec);
            if(!overwrite && ((!exists && !doc.DiskBytes.empty()) || (exists && Read(fs::u8path(doc.Path))!=doc.DiskBytes))){doc.Conflict=true;Status="File changed outside the IDE. Reload or explicitly overwrite before saving.";return false;}
            auto bytes=doc.Bytes();if(!AtomicFile::WriteBytes(fs::u8path(doc.Path),bytes,true)){Status="Save failed: "+doc.Path;return false;}
            doc.DiskBytes=bytes;doc.Saved=doc.Editor.GetText();doc.Conflict=false;Status="Saved "+fs::u8path(doc.Path).filename().u8string();Scripting::RequestBuild();Queue();Persist();return true;
        }catch(const std::exception& e){Status=e.what();return false;}
    }
    bool SaveAll() {bool okay=true;for(auto& d:Documents)if(d->Dirty())okay=Save(*d)&&okay;return okay;}
    void Close(const std::string& path) {
        auto found=std::find_if(Documents.begin(),Documents.end(),[&](const auto& d){return Same(d->Path,path);});
        if(found==Documents.end())return;
        if(Active==found->get())Active=nullptr;Documents.erase(found);
        if(!Active && !Documents.empty()){Active=Documents.front().get();Activate=Active->Path;}
        Queue();Persist();
    }
    void Reload(Document& doc) {
        try {
            auto bytes=Read(fs::u8path(doc.Path));
            if(bytes.size()>8*1024*1024 || bytes.find('\0')!=std::string::npos)throw std::runtime_error("Disk source must be UTF-8 and no larger than 8 MB.");
            auto disk=bytes;bool bom=bytes.compare(0,3,"\xEF\xBB\xBF")==0;if(bom)bytes.erase(0,3);Wide(bytes);
            doc.DiskBytes=std::move(disk);doc.Bom=bom;doc.CRLF=bytes.find("\r\n")!=std::string::npos;doc.Editor.SetText(bytes);doc.Saved=doc.Editor.GetText();doc.Conflict=false;Queue();Persist();
        }catch(const std::exception& e){doc.Conflict=true;Status=e.what();}
    }
    void Poll() {
        if(FileScan.valid() && FileScan.wait_for(std::chrono::milliseconds(0))==std::future_status::ready)try {ProjectFiles=FileScan.get();}catch(const std::exception& e){Status=e.what();}
        if(!FileScan.valid() && Clock::now()-LastScan>std::chrono::seconds(3)) {
            LastScan=Clock::now();auto root=ProjectPaths::Resolve("assets");
            FileScan=std::async(std::launch::async,[root]{std::vector<std::string> paths;for(const auto& entry:fs::recursive_directory_iterator(fs::u8path(root)))if(entry.path().extension()==".cs")paths.push_back(entry.path().u8string());std::sort(paths.begin(),paths.end());return paths;});
        }
        if(Process && WaitForSingleObject(Process,0)==WAIT_OBJECT_0) {
            CloseHandle(Process);Process=nullptr;
            try {auto result=Json::parse(Read(ResponsePath));
                const bool positionMatches=RunningMode=="analyze" || RunningMode=="format" || (RunningMode=="hover" ? HoverActive && HoverPosition==RunningCursor : Active && Active->Editor.GetCursorPosition()==RunningCursor);
                if(Active && positionMatches && Same(Active->Path,RunningPath) && Active->Editor.GetText()==RunningText) {
                    Result=std::move(result);AnalysisError=Result.value("error",std::string());
                    HoverInfo=RunningMode=="hover";
                    TextEditor::ErrorMarkers markers;for(const auto& d:Result.value("diagnostics",Json::array()))if(d["location"].is_object() && Same(d["location"]["path"].get<std::string>(),Active->Path))markers[d["location"]["line"].get<int>()]+=d["code"].get<std::string>()+": "+d["message"].get<std::string>()+"\n";
                    Active->Editor.SetErrorMarkers(markers);
                    std::vector<TextEditor::SemanticHighlight> highlights;
                    using P=TextEditor::PaletteIndex;
                    static const std::map<std::string,P> colors{{"type",P::TypeName},{"method",P::MethodName},{"property",P::PropertyName},{"field",P::FieldName},{"parameter",P::ParameterName},{"local",P::LocalName},{"namespace",P::NamespaceName},{"enumMember",P::EnumMember}};
                    if(Result.contains("highlights") && Result["highlights"].is_array())
                        for(const auto& h:Result["highlights"]) {
                            auto color=colors.find(h.value("kind",std::string()));
                            if(color!=colors.end())highlights.push_back({h.at("line").get<int>(),h.at("start").get<int>(),h.at("length").get<int>(),color->second});
                        }
                    Active->Editor.SetSemanticHighlights(highlights);
                    if(RunningMode=="format" && Result["formatted"].is_string()){Active->Editor.SelectAll();Active->Editor.ReplaceSelection(Result["formatted"].get<std::string>());Queue();Persist();}
                    if(RunningMode=="rename"){RenamePreview=true;}
                    if(RunningMode=="definition") {const auto locations=Result.value("definitions",Json::array());if(!locations.empty())Open(locations[0]["path"],locations[0]["line"],locations[0]["column"]);else Status="No source definition; use Symbol info for referenced API signatures.";}
                    if(RunningMode=="complete"){CompletionOpen=true;CompletionIndex=0;}
                }
            }catch(const std::exception& e){AnalysisError=e.what();}
        }
        if(Process && Clock::now()-Started>std::chrono::seconds(20)){TerminateProcess(Process,1);CloseHandle(Process);Process=nullptr;AnalysisError="C# analysis timed out. Use Analyze to retry.";}
        if(!Process && NeedAnalysis && Active && Clock::now()-Changed>std::chrono::milliseconds(PendingMode=="analyze"?450:0))Start();
        if(Clock::now()-LastDiskCheck>std::chrono::seconds(1)) {
            LastDiskCheck=Clock::now();for(auto& d:Documents)try {
                auto disk=Read(fs::u8path(d->Path));if(disk!=d->DiskBytes){if(d->Dirty())d->Conflict=true;else Reload(*d);}
            }catch(...){d->Conflict=true;}
            BuildProblems=Json::array();std::set<std::string> seen;
            static const std::regex diagnostic(R"(^\s*(.+?)\((\d+),(\d+)\):\s*(error|warning)\s+(\w+):\s*(.+))");
            for(const auto& name:{"build.log","editor-build.log"})try {
                auto log=Read(fs::u8path(ProjectPaths::Resolve(std::string("Scripts/")+name)));BuildLogs[name]=log;
                std::istringstream lines(log);std::string line;while(std::getline(lines,line)){std::smatch match;if(!std::regex_match(line,match,diagnostic) || !seen.insert(line).second)continue;
                    BuildProblems.push_back({{"severity",match[4].str()},{"code",match[5].str()},{"message",match[6].str()},
                        {"location",{{"path",Canonical(match[1].str())},{"line",std::stoi(match[2].str())},{"column",std::stoi(match[3].str())}}}});
                }
            }catch(...){}
        }
    }
    Clock::time_point Started;
    void Start() {
        NeedAnalysis=false;RunningPath=Active->Path;RunningText=Active->Editor.GetText();RunningMode=PendingMode;
        const auto cursor=RunningMode=="hover"?HoverPosition:Active->Editor.GetCursorPosition();const auto lines=Active->Editor.GetTextLines();
        RunningCursor=cursor;
        const int byte=Active->Editor.ByteIndexAt(cursor);RunningLine=cursor.mLine+1;RunningColumn=static_cast<int>(Wide(lines[cursor.mLine].substr(0,byte)).size())+1;
        Json buffers=Json::array();for(const auto& d:Documents)buffers.push_back({{"path",d->Path},{"text",d->Editor.GetText()}});
        const auto managed=fs::u8path(EnginePaths::ExeDir())/"Managed";
        const auto helper=managed/"IDE/Tartarus.CodeAnalysis.exe";
        if(!fs::is_regular_file(helper)){AnalysisError="Roslyn helper is missing. Rebuild the engine's TartarusScripts target.";return;}
        if(!AtomicFile::WriteBytes(RequestPath,Json{{"root",ProjectPaths::Root()},{"file",RunningPath},{"runtime",(managed/"Tartarus.Runtime.dll").u8string()},{"buffers",buffers},{"line",RunningLine},{"column",RunningColumn},{"mode",RunningMode},{"newName",NewName}}.dump())){AnalysisError="Cannot write analysis request.";return;}
        std::error_code ec;fs::remove(ResponsePath,ec);
        std::wstring command=L"\""+helper.wstring()+L"\" \""+RequestPath.wstring()+L"\" \""+ResponsePath.wstring()+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        if(!CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)){AnalysisError="Cannot start C# analysis (Windows error "+std::to_string(GetLastError())+").";return;}
        CloseHandle(process.hThread);Process=process.hProcess;Started=Clock::now();AnalysisError.clear();
    }
    void ApplyCompletion(const std::string& name) {
        if(!Active)return;auto pos=Active->Editor.GetCursorPosition();auto line=Active->Editor.GetCurrentLineText();int byte=Active->Editor.CursorByteIndex(),start=byte;
        while(start>0 && (isalnum(static_cast<unsigned char>(line[start-1])) || line[start-1]=='_'))--start;
        auto begin=pos;begin.mColumn-=byte-start;Active->Editor.SetSelection(begin,pos);Active->Editor.ReplaceSelection(name);CompletionOpen=false;Queue();Persist();
    }
    void FindNext(bool reverse=false) {
        if(!Active || Find.empty())return;auto text=Active->Editor.GetText();auto lines=Active->Editor.GetTextLines();auto c=Active->Editor.GetCursorPosition();size_t offset=Active->Editor.CursorByteIndex();for(int i=0;i<c.mLine;++i)offset+=lines[i].size()+1;
        size_t found=reverse?text.rfind(Find,offset>0?offset-1:0):text.find(Find,offset);if(found==std::string::npos)found=reverse?text.rfind(Find):text.find(Find);
        if(found==std::string::npos){Status="No match.";return;}
        int row=static_cast<int>(std::count(text.begin(),text.begin()+found,'\n'));auto last=text.rfind('\n',found);size_t byte=found-(last==std::string::npos?0:last+1);
        int col=static_cast<int>(Wide(lines[row].substr(0,byte)).size())+1;Jump(*Active,row+1,col);auto begin=Active->Editor.GetCursorPosition();Jump(*Active,row+1,col+static_cast<int>(Wide(Find).size()));Active->Editor.SetSelection(begin,Active->Editor.GetCursorPosition());
    }
    void ApplyRename() {
        try {
            std::map<std::string,Json> groups;for(const auto& edit:Result.value("edits",Json::array()))groups[edit.at("path").get<std::string>()].push_back(edit);
            std::map<std::string,std::string> updated;
            // Validate every pre-image before applying any buffer edits.
            for(auto& [path,edits]:groups) {
                Document* doc=nullptr;for(auto& d:Documents)if(Same(path,d->Path))doc=d.get();
                std::string current=doc?doc->Editor.GetText():Read(fs::u8path(path));if(current.compare(0,3,"\xEF\xBB\xBF")==0)current.erase(0,3);
                const auto before=edits[0].at("before").get<std::string>();if(current!=before)throw std::runtime_error("Source changed during rename. Request a fresh preview.");
                auto text=Wide(current);std::sort(edits.begin(),edits.end(),[](const Json& a,const Json& b){return a["start"].get<int>()>b["start"].get<int>();});
                for(const auto& edit:edits)text.replace(edit["start"].get<size_t>(),edit["length"].get<size_t>(),Wide(edit["text"].get<std::string>()));updated[path]=Utf8(text);
            }
            for(const auto& [path,text]:updated)if(auto* doc=Open(path)){doc->Editor.SelectAll();doc->Editor.ReplaceSelection(text);}
            RenamePreview=false;Status="Rename applied to buffers. Review and Save All to compile.";Queue();Persist();
        }catch(const std::exception& e){Status=e.what();}
    }
    void SearchProject() {
        SearchResults=Json::array();if(ProjectSearch.empty())return;
        try {for(const auto& entry:fs::recursive_directory_iterator(fs::u8path(ProjectPaths::Resolve("assets"))))if(entry.path().extension()==".cs") {
            std::string text;for(const auto& d:Documents)if(Same(d->Path,entry.path().u8string())){text=d->Editor.GetText();break;}if(text.empty())text=Read(entry.path());
            size_t offset=0;while((offset=text.find(ProjectSearch,offset))!=std::string::npos && SearchResults.size()<500){int row=static_cast<int>(std::count(text.begin(),text.begin()+offset,'\n'))+1;auto start=text.rfind('\n',offset);start=start==std::string::npos?0:start+1;SearchResults.push_back({{"path",entry.path().u8string()},{"line",row},{"column",static_cast<int>(Wide(text.substr(start,offset-start)).size())+1}});offset+=ProjectSearch.size();}
        }}catch(const std::exception& e){Status=e.what();}
    }
    void Locations(const Json& places) {int id=0;for(const auto& place:places){ImGui::PushID(++id);auto path=place.at("path").get<std::string>();auto label=fs::u8path(path).filename().u8string()+":"+std::to_string(place["line"].get<int>());if(ImGui::Selectable(label.c_str()))Open(path,place["line"],place["column"]);ImGui::PopID();}}
    void Draw();
};
ScriptIDE::ScriptIDE():impl(std::make_unique<Impl>()){}
ScriptIDE::~ScriptIDE()=default;
void ScriptIDE::Open(const std::string& path,int line,int column){impl->Open(path,line,column);}
void ScriptIDE::Show(){impl->Visible=impl->Focus=true;}
void ScriptIDE::Draw(unsigned int sceneDockId){impl->SceneDockId=sceneDockId;impl->Draw();}
bool ScriptIDE::HasUnsavedFiles() const {return std::any_of(impl->Documents.begin(),impl->Documents.end(),[](const auto& d){return d->Dirty();});}
bool ScriptIDE::OwnsKeyboard() const {return impl->KeyboardActive;}
bool ScriptIDE::SaveAll(){return impl->SaveAll();}
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
            if(ImGui::MenuItem("Undo","Ctrl+Z",false,Active && Active->Editor.CanUndo())){Active->Editor.Undo();Queue();Persist();}
            if(ImGui::MenuItem("Redo","Ctrl+Y",false,Active && Active->Editor.CanRedo())){Active->Editor.Redo();Queue();Persist();}
            if(ImGui::MenuItem("Find / Replace","Ctrl+F"))FindVisible=true;
            if(ImGui::MenuItem("Format Document","Ctrl+Shift+I",false,Active && !Active->Editor.IsReadOnly()))Queue("format");ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("Navigate")) {
            if(ImGui::MenuItem("Go to line","Ctrl+G"))GoTo=true;
            if(ImGui::MenuItem("Go to definition","F12",false,Active!=nullptr))Queue("definition");
            if(ImGui::MenuItem("Find references","Shift+F12",false,Active!=nullptr))Queue("references");
            if(ImGui::MenuItem("Rename symbol","F2",false,Active && !Active->Editor.IsReadOnly()))RenamePrompt=true;
            if(ImGui::MenuItem("Find in project","Ctrl+Shift+F"))ProjectFind=true;ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("Build")) {if(ImGui::MenuItem("Save All & Compile","F6")){if(SaveAll())Scripting::RequestBuild();}if(ImGui::MenuItem("Analyze buffers"))Queue();ImGui::EndMenu();}
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
        if(ImGui::IsKeyPressed(ImGuiKey_F12,false))Queue(io.KeyShift?"references":"definition");
        if(ImGui::IsKeyPressed(ImGuiKey_F2,false))RenamePrompt=true;
        if(io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_I,false))Queue("format");
        if(ImGui::IsKeyPressed(ImGuiKey_F6,false)){if(SaveAll())Scripting::RequestBuild();}
        if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space,false))Queue("complete");
    }
    const float scale=EditorTheme::Scale(),divider=6.0f*scale;
    const auto space=ImGui::GetContentRegionAvail();
    const float sidebarMin=std::min(80.0f*scale,std::max(1.0f,space.x*.25f));
    const float sidebarMax=std::max(sidebarMin,space.x-divider-std::min(180.0f*scale,space.x*.5f));
    const float sidebar=std::clamp(SidebarWidth*scale,sidebarMin,sidebarMax);
    ImGui::BeginChild("Sources",ImVec2(sidebar,0),ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted("PROJECT SCRIPTS");
    Field("##ScriptSearch",SourceFilter,sidebar-20);
    try {for(const auto& path:ProjectFiles) {
        auto label=fs::u8path(ProjectPaths::Relativize(path)).generic_u8string();
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
        Field("Find",Find,180);ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Previous"))FindNext(true);ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Next"))FindNext();ImGui::SameLine();ImGui::Checkbox("Replace",&ReplaceVisible);ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Close find"))FindVisible=false;
        if(ReplaceVisible && Active && !Active->Editor.IsReadOnly()){Field("With",Replace,180);ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Replace match") && Active->Editor.GetSelectedText()==Find){Active->Editor.ReplaceSelection(Replace);Queue();Persist();}ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Replace all") && !Find.empty()){auto text=Active->Editor.GetText();size_t p=0;while((p=text.find(Find,p))!=std::string::npos){text.replace(p,Find.size(),Replace);p+=Replace.size();}Active->Editor.SelectAll();Active->Editor.ReplaceSelection(text);Queue();Persist();}}
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
                if(Active!=&d)Queue();Active=&d;if(Same(Activate,d.Path))Activate.clear();
                auto cursor=d.Editor.GetCursorPosition();ImGui::TextDisabled("%s  |  Ln %d, Col %d%s",ProjectPaths::Relativize(d.Path).c_str(),cursor.mLine+1,cursor.mColumn+1,d.Editor.IsReadOnly()?"  |  read-only":"");
                const auto before=d.Editor.GetText();
                d.Editor.SetPalette(ScriptIDEThemes::Palette(Theme));
                EditorTheme::PushMono();d.Editor.Render("Code",ImVec2(0,std::max(1.0f,ImGui::GetContentRegionAvail().y)));EditorTheme::PopFont();
                TextEditor::Coordinates hovered;
                if(d.Editor.HoveredCoordinates(hovered)) {
                    if(!HoverActive || hovered!=HoverPosition){HoverPosition=hovered;HoverStarted=Clock::now();HoverInfo=false;}
                    HoverActive=true;
                    if(HoverInfo){if(!Result.value("info",std::string()).empty())ImGui::SetTooltip("%s",Result.value("info",std::string()).c_str());}
                    else if(!NeedAnalysis && !Process && Clock::now()-HoverStarted>std::chrono::milliseconds(650))Queue("hover");
                }else {HoverActive=false;HoverInfo=false;}
                if(d.Editor.GetText()!=before){Queue();CompletionOpen=false;Persist();}
                else if(d.Editor.IsCursorPositionChanged() && PendingMode=="analyze")Queue();
                if(!before.empty() && d.Editor.IsTextChanged()){auto line=d.Editor.GetCurrentLineText();int index=d.Editor.CursorByteIndex();if(index>0 && line[index-1]=='.')Queue("complete");}
                ImGui::EndTabItem();
            }
            if(!open)ClosePath=d.Path;
        }
        ImGui::EndTabBar();
    }
    if(!Active)ImGui::TextWrapped("Double-click a .cs asset or choose a project script. Ctrl+Space completes symbols; F12 opens definitions; F6 saves and compiles.");
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
    ImGui::TextDisabled("%s%s",Scripting::Building()?"Compiling scripts...":Scripting::BuildPending()?"Compile queued":compileErrors?"Compiler errors - check Problems":"Scripts ready",Process?"  |  Roslyn analyzing...":"");
    if(!AnalysisError.empty())ImGui::TextWrapped("Analysis: %s",AnalysisError.c_str());
    if(!Status.empty())ImGui::TextWrapped("%s",Status.c_str());
    if(ImGui::BeginTabBar("IDEInformation")) {
        if(ImGui::BeginTabItem("Problems")) {
            ImGui::BeginChild("Diagnostics",ImVec2(0,0));int id=0;
            ImGui::TextDisabled("LIVE BUFFER ANALYSIS");
            for(const auto& problem:Result.value("diagnostics",Json::array())){ImGui::PushID(++id);auto label=problem["severity"].get<std::string>()+" "+problem["code"].get<std::string>()+": "+problem["message"].get<std::string>();if(ImGui::Selectable(label.c_str()) && problem["location"].is_object()){const auto& l=problem["location"];Open(l["path"],l["line"],l["column"]);}ImGui::PopID();}
            ImGui::TextDisabled("LAST COMPILER OUTPUT");
            for(const auto& problem:BuildProblems){ImGui::PushID(++id);auto label=problem["severity"].get<std::string>()+" "+problem["code"].get<std::string>()+": "+problem["message"].get<std::string>();if(ImGui::Selectable(label.c_str())){const auto& l=problem["location"];Open(l["path"],l["line"],l["column"]);}ImGui::PopID();}
            ImGui::EndChild();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Symbol info")){ImGui::BeginChild("Signature",ImVec2(0,0));ImGui::TextWrapped("%s",Result.value("info",std::string()).c_str());Locations(Result.value("definitions",Json::array()));ImGui::EndChild();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("References")){ImGui::BeginChild("ReferenceList",ImVec2(0,0));Locations(Result.value("references",Json::array()));ImGui::EndChild();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Build output")){ImGui::BeginChild("BuildLog",ImVec2(0,0));for(const auto& [name,log]:BuildLogs){ImGui::TextUnformatted(name.c_str());ImGui::TextUnformatted(log.c_str());}ImGui::EndChild();ImGui::EndTabItem();}
        ImGui::EndTabBar();
    }
    ImGui::EndChild(); // InformationPane
    ImGui::EndChild();
    if(CompletionOpen){ImGui::OpenPopup("Completion");CompletionOpen=false;}
    if(ImGui::BeginPopup("Completion")) {
        Field("Filter",CompletionFilter,220);ImGui::BeginChild("Candidates",ImVec2(470,220));int id=0;
        for(const auto& item:Result.value("completions",Json::array())){const auto name=item["name"].get<std::string>();if(!CompletionFilter.empty() && name.find(CompletionFilter)==std::string::npos)continue;ImGui::PushID(++id);if(ImGui::Selectable((name+"   "+item["kind"].get<std::string>()).c_str())){ApplyCompletion(name);CompletionFilter.clear();ImGui::CloseCurrentPopup();}if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",item["detail"].get<std::string>().c_str());ImGui::PopID();}ImGui::EndChild();ImGui::EndPopup();
    }
    if(GoTo){ImGui::OpenPopup("Go to line");GoTo=false;}
    if(ImGui::BeginPopup("Go to line")){ImGui::InputInt("Line",&GoLine);if(EditorUIPrimitives::PrimaryButton("Go") && Active){Jump(*Active,GoLine,1);ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
    if(RenamePrompt){ImGui::OpenPopup("Rename symbol");RenamePrompt=false;}
    if(ImGui::BeginPopup("Rename symbol")){Field("New name",NewName,220);if(EditorUIPrimitives::PrimaryButton("Preview rename")){Queue("rename");ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
    if(RenamePreview){ImGui::OpenPopup("Rename preview");RenamePreview=false;}
    if(ImGui::BeginPopupModal("Rename preview",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::TextUnformatted("Review references; Apply changes buffers until you Save All.");Locations(Result.value("references",Json::array()));if(EditorUIPrimitives::PrimaryButton("Apply",ImVec2(100,0))){ApplyRename();ImGui::CloseCurrentPopup();}ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Cancel",ImVec2(100,0)))ImGui::CloseCurrentPopup();ImGui::EndPopup();}
    if(ProjectFind){ImGui::OpenPopup("Find in project");ProjectFind=false;}
    if(ImGui::BeginPopup("Find in project")){Field("Text",ProjectSearch,300);ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Search"))SearchProject();ImGui::BeginChild("ProjectMatches",ImVec2(560,280));Locations(SearchResults);ImGui::EndChild();ImGui::EndPopup();}
    if(!ClosePath.empty()) {
        bool reload=ClosePath.rfind("reload:",0)==0;auto path=reload?ClosePath.substr(7):ClosePath;Document* closing=nullptr;for(auto& d:Documents)if(Same(d->Path,path))closing=d.get();
        if(closing && closing->Dirty())ImGui::OpenPopup("Unsaved script");else {if(closing){if(reload)Reload(*closing);else Close(path);}ClosePath.clear();}
    }
    if(ImGui::BeginPopupModal("Unsaved script",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Save changes before closing or reloading this script?");
        if(EditorUIPrimitives::PrimaryButton("Save")){auto path=ClosePath.rfind("reload:",0)==0?ClosePath.substr(7):ClosePath;for(auto& d:Documents)if(Same(d->Path,path) && Save(*d)){if(ClosePath.rfind("reload:",0)!=0)Close(path);ClosePath.clear();ImGui::CloseCurrentPopup();break;}}
        ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Discard")){if(ClosePath.rfind("reload:",0)==0){auto path=ClosePath.substr(7);for(auto& d:Documents)if(Same(d->Path,path))Reload(*d);}else Close(ClosePath);ClosePath.clear();ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Cancel")){ClosePath.clear();ImGui::CloseCurrentPopup();}ImGui::EndPopup();
    }
    ImGui::End();
}
