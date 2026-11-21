#include "UnitTestSupport.h"
#include "ImCurveAdapter.h"
#include "CurveEditor.h"
#include "EditorFileHistory.h"
#include "UndoDeltaChain.h"
#include "EditorLayer.h"
#include "EditorSearch.h"
#include "LruMap.h"
#include "RestartState.h"
#include "EditorRestart.h"
#include "EditorFileStat.h"
#include "Core/Env.h"
#include "World.h"
#include "AssetLibrary.h"
#include "SceneSerializer.h"
#include "OutfitSystem.h"
#include "Components.h"
#include "Profiler.h"
#include "Core/Paths.h"
#include "ProjectSettings.h"
#include <filesystem>
#include <fstream>
#include "UndoTrigger.h"
#include "EditorSettings.h"
#include "../../extern/ImGuiColorTextEdit/TextEditor.h"
#include "../../extern/imcurve/imcurve_editor.hpp"
#include <imgui_internal.h>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <cstring>
#include <cstdio>

// Exercise the real transaction code without starting the renderer or writing user preferences.
struct EditorHistoryTestAccess {
    static const std::vector<entt::entity>& Order(const EditorLayer& e,const World& w) {return e.HierarchyCreationOrder(w);}
    static int Ordinal(const EditorLayer& e,const World& w,entt::entity x) {return e.HierarchyCreationOrdinal(w,x);}
    static std::vector<entt::entity> Siblings(const EditorLayer& e,const World& w,entt::entity p) {return e.HierarchySiblingsInOrder(w,p);}
    static void Reorder(EditorLayer& e,World& w,std::vector<entt::entity> m,entt::entity anchor,bool after) {e.ReorderHierarchySiblings(w,m,anchor,after,false);}
    static void Begin(EditorLayer& e,World& w,AssetLibrary& a) {e.m_AssetsPtr=&a;e.BeginGlobalUndoFrame(w);}
    static void Finish(EditorLayer& e,World& w) {e.FinishGlobalUndo(w,true);AtomicFile::SetChangeObserver({});}
    static void Edit(EditorLayer& e,World& w) {e.PushUndo(w,"Test edit");}
    static void Stage(EditorLayer& e,World& w) {e.StageUndo(w);}
    static void Commit(EditorLayer& e,World& w) {e.CommitStagedUndo(w,"Test drag");}
    static bool Active(const EditorLayer& e) {return e.m_GlobalUndoActive;}
    static size_t Count(const EditorLayer& e) {return e.m_UndoStack.size();}
    static const std::string& Before(const EditorLayer& e) {return e.m_UndoBaseJson;}
    static const std::string& Current(const EditorLayer& e) {return e.m_CurrentUndoScene;}
    static void Clear(EditorLayer& e) {e.ClearUndoHistory();}
    static std::string TopLabel(const EditorLayer& e) {return e.m_UndoStack.empty()?std::string():e.m_UndoStack.back().Label;}
    static void Undo(EditorLayer& e,World& w,AssetLibrary& a) {e.Undo(w,a);}
    static void Redo(EditorLayer& e,World& w,AssetLibrary& a) {e.Redo(w,a);}
    static void Select(EditorLayer& e,World& w,entt::entity entity) {
        e.m_EditPushedThisFrame=false;e.m_SelHistoryNavigating=false;
        e.m_Selected=entity;e.RecordSelectionHistory(w);
    }
};

namespace {
void TestEditorNavigationUndo() {
    auto* previous=ImGui::GetCurrentContext();auto* context=ImGui::CreateContext();
    auto& io=ImGui::GetIO();io.DisplaySize={800,600};io.DeltaTime=1.0f/60;io.IniFilename=nullptr;
    unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    World world;AssetLibrary assets;
    for(int i=0;i<1500;++i)world.CreateEmptyEntity({}, {}, {1,1,1},"Navigation fixture "+std::to_string(i));
    const auto start=std::chrono::steady_clock::now();
    const auto expected=SceneSerializer::SaveToString(world,assets);
    const auto second=SceneSerializer::SaveToString(world,assets);
    const auto oldMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    {
        EditorLayer editor;
        ImGui::NewFrame();EditorHistoryTestAccess::Begin(editor,world,assets);EditorHistoryTestAccess::Finish(editor,world);ImGui::Render();
        CHECK(EditorHistoryTestAccess::Current(editor)==expected);
        Profiler::BeginFrame();
        const auto navStart=std::chrono::steady_clock::now();
        for(int frame=0;frame<120;++frame) {
            io.AddMousePosEvent(100.0f+static_cast<float>(frame),100);io.AddMouseButtonEvent(1,frame%2==0);
            io.AddKeyEvent(ImGuiKey_W,frame%2==0);io.AddMouseWheelEvent(0,1);
            ImGui::NewFrame();EditorHistoryTestAccess::Begin(editor,world,assets);
            CHECK(!EditorHistoryTestAccess::Active(editor));
            EditorHistoryTestAccess::Finish(editor,world);ImGui::Render();
        }
        const auto navMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-navStart).count()/120;
        Profiler::BeginFrame();
        for(const auto& sample:Profiler::GetLastFrame())CHECK(sample.Name!="Undo Scene Snapshot");
        CHECK(EditorHistoryTestAccess::Count(editor)==0);
        std::cout<<"[EditorHitch] 1500 entities: old input snapshot pair="<<oldMs<<"ms, navigation frame="<<navMs<<"ms (120 frames, zero scene snapshots)\n";

        // A checkbox-style control requests undo AFTER changing its value.
        ImGui::NewFrame();EditorHistoryTestAccess::Begin(editor,world,assets);
        world.Render.ExposureEV+=1;EditorHistoryTestAccess::Edit(editor,world);EditorHistoryTestAccess::Finish(editor,world);ImGui::Render();
        CHECK(EditorHistoryTestAccess::Count(editor)==1);CHECK(EditorHistoryTestAccess::Before(editor)==expected);
        const auto changed=SceneSerializer::SaveToString(world,assets);CHECK(EditorHistoryTestAccess::Current(editor)==changed);
        EditorHistoryTestAccess::Undo(editor,world,assets);CHECK(SceneSerializer::SaveToString(world,assets)==expected);
        EditorHistoryTestAccess::Redo(editor,world,assets);CHECK(SceneSerializer::SaveToString(world,assets)==changed);
        CHECK(EditorHistoryTestAccess::Current(editor)==changed);

        // A drag across frames coalesces to one entry, and an unchanged activation keeps redo.
        const auto count=EditorHistoryTestAccess::Count(editor);
        ImGui::NewFrame();EditorHistoryTestAccess::Begin(editor,world,assets);EditorHistoryTestAccess::Stage(editor,world);
        world.Render.ExposureEV+=.25f;AtomicFile::SetChangeObserver({});ImGui::Render();
        ImGui::NewFrame();EditorHistoryTestAccess::Begin(editor,world,assets);world.Render.ExposureEV+=.25f;
        EditorHistoryTestAccess::Commit(editor,world);EditorHistoryTestAccess::Finish(editor,world);ImGui::Render();
        CHECK(EditorHistoryTestAccess::Count(editor)==count+1);CHECK(EditorHistoryTestAccess::Before(editor)==changed);
        EditorHistoryTestAccess::Undo(editor,world,assets);
        ImGui::NewFrame();EditorHistoryTestAccess::Begin(editor,world,assets);EditorHistoryTestAccess::Stage(editor,world);
        EditorHistoryTestAccess::Finish(editor,world);ImGui::Render();CHECK(EditorHistoryTestAccess::Count(editor)==count);
        EditorHistoryTestAccess::Redo(editor,world,assets);CHECK(world.Render.ExposureEV>nlohmann::json::parse(changed).value("exposureEV",0.0f));

        // The first selection entry needs no serialized scene anchor, but real edits after it do.
        EditorHistoryTestAccess::Clear(editor);
        auto entity=*world.Registry.view<OrderComponent>().begin();EditorHistoryTestAccess::Select(editor,world,entity);
        CHECK(EditorHistoryTestAccess::Before(editor)=="{}");
        ImGui::NewFrame();EditorHistoryTestAccess::Begin(editor,world,assets);
        const auto preEdit=SceneSerializer::SaveToString(world,assets);world.Render.ExposureEV+=1;
        EditorHistoryTestAccess::Edit(editor,world);EditorHistoryTestAccess::Finish(editor,world);ImGui::Render();
        EditorHistoryTestAccess::Undo(editor,world,assets);CHECK(SceneSerializer::SaveToString(world,assets)==preEdit);
        EditorHistoryTestAccess::Undo(editor,world,assets);CHECK(SceneSerializer::SaveToString(world,assets)==preEdit);

        // A file-only transaction, followed by a mixed file/scene transaction.
        const auto path=std::filesystem::path(ProjectPaths::Resolve("undo-hitch-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".txt"));
        CHECK(AtomicFile::WriteBytes(path,"before",true));
        ImGui::NewFrame();EditorHistoryTestAccess::Begin(editor,world,assets);Profiler::BeginFrame();
        CHECK(AtomicFile::WriteBytes(path,"after",true));EditorHistoryTestAccess::Finish(editor,world);ImGui::Render();Profiler::BeginFrame();
        for(const auto& sample:Profiler::GetLastFrame())CHECK(sample.Name!="Undo Scene Snapshot");
        EditorHistoryTestAccess::Undo(editor,world,assets);CHECK(*EditorFileHistory::Capture(path).Bytes=="before");
        EditorHistoryTestAccess::Redo(editor,world,assets);CHECK(*EditorFileHistory::Capture(path).Bytes=="after");
        ImGui::NewFrame();EditorHistoryTestAccess::Begin(editor,world,assets);
        CHECK(AtomicFile::WriteBytes(path,"mixed",true));world.Render.ExposureEV+=1;EditorHistoryTestAccess::Edit(editor,world);
        EditorHistoryTestAccess::Finish(editor,world);ImGui::Render();EditorHistoryTestAccess::Undo(editor,world,assets);
        CHECK(*EditorFileHistory::Capture(path).Bytes=="after");CHECK(SceneSerializer::SaveToString(world,assets)==preEdit);
        // A file the next step restores can't be read any more (a folder now): Undo logs and keeps
        // the history instead of throwing out of the frame.
        std::filesystem::remove(path);std::filesystem::create_directory(path);
        const size_t undoDepth=EditorHistoryTestAccess::Count(editor);
        bool threw=false;
        try{EditorHistoryTestAccess::Undo(editor,world,assets);}catch(...){threw=true;}
        CHECK(!threw);CHECK(EditorHistoryTestAccess::Count(editor)==undoDepth);
        std::filesystem::remove(path);
    }
    AtomicFile::SetChangeObserver({});ImGui::DestroyContext(context);ImGui::SetCurrentContext(previous);
}
void TestPrefabToolbarReturn() {
    auto* previous=ImGui::GetCurrentContext(); auto* context=ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.DisplaySize={800,600}; io.DeltaTime=1.0f/60;
    io.IniFilename=nullptr;
    unsigned char* pixels=nullptr; int width=0,height=0;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    const auto path=std::filesystem::temp_directory_path()/"tartarus-prefab-toolbar-test.prefab";
    { std::ofstream file(path); file << R"({"formatVersion":4,"models":[],"boxes":[],"empties":[{"id":0,"name":"Prefab","position":[0,0,0]}]})"; }
    {
        World world; AssetLibrary assets; EditorLayer editor;
        world.CreateEmptyEntity({1,2,3},{0,0,0},{1,1,1},"Scene sentinel");
        auto frame=[&](bool scene) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0,30},ImGuiCond_Always);
            ImGui::SetNextWindowSize({800,570},ImGuiCond_Always);
            ImGui::Begin("##DockHost",nullptr,ImGuiWindowFlags_NoSavedSettings);
            ImGui::BeginChild(scene?"Scene":"Script IDE");
            ImGui::Dummy({600,400}); ImGui::EndChild(); ImGui::End();
            editor.DrawViewportActionBar(world,assets,false,false,false);
            ImGui::Render();
        };
        // A previously shown Play toolbar must become inactive as soon as a prefab opens.
        ImGui::NewFrame(); ImGui::Begin("##ViewportActionBar"); ImGui::End(); ImGui::Render();
        editor.EnterPrefabMode(world,assets,path.u8string()); CHECK(editor.InPrefabMode());
        frame(true); frame(true);
        auto* prefabBar=ImGui::FindWindowByName("##PrefabModeBar");
        CHECK(prefabBar && prefabBar->Active && !prefabBar->Hidden);
        CHECK(!ImGui::FindWindowByName("##ViewportActionBar")->Active);
        // Returning must remain possible when neither Scene nor Game is visible.
        frame(false); frame(false);
        CHECK(prefabBar->Active && !prefabBar->Hidden && prefabBar->Pos.y>=editor.ToolbarHeightPx());
        const ImVec2 backButton(prefabBar->Pos.x+20,prefabBar->Pos.y+ImGui::GetStyle().WindowPadding.y+ImGui::GetFrameHeight()*.5f);
        io.AddMousePosEvent(backButton.x,backButton.y); frame(false);
        io.AddMouseButtonEvent(0,true); frame(false);
        io.AddMouseButtonEvent(0,false); frame(false);
        CHECK(!editor.InPrefabMode());
        CHECK(!ImGui::FindWindowByName("##ViewportActionBar")->Active); // no overlapping bar on exit
        bool restored=false;
        for(auto [e,name]:world.Registry.view<NameComponent>().each()) restored=restored || name.Name=="Scene sentinel";
        CHECK(restored);
        if(editor.InPrefabMode()) editor.ExitPrefabMode(world,assets,false);
    }
    std::filesystem::remove(path);
    ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous);
}

void TestScriptIDETextEditing() {
    auto* previous=ImGui::GetCurrentContext();auto* context=ImGui::CreateContext();
    TextEditor editor;
    const std::string original="// UTF-8: \xF0\x9F\x8E\xAF\nclass Probe { float Speed=1; }\n";
    editor.SetText(original);CHECK(editor.GetText()==original);
    editor.SetText("Engine.Pos");editor.SetCursorPosition({0,10});editor.SetSelection({0,7},{0,10});
    editor.ReplaceSelection("Position");CHECK(editor.GetText()=="Engine.Position");CHECK(editor.CanUndo());
    editor.Undo();CHECK(editor.GetText()=="Engine.Pos");editor.Redo();CHECK(editor.GetText()=="Engine.Position");
    editor.SelectAll();editor.ReplaceSelection(original);CHECK(editor.GetText()==original);editor.Undo();CHECK(editor.GetText()=="Engine.Position");
    editor.SetReadOnly(true);editor.SelectAll();editor.ReplaceSelection("overwrite");CHECK(editor.GetText()=="Engine.Position");
    editor.SetReadOnly(false);editor.SetText("\tEngine.Pos");editor.SetCursorPosition({0,14});CHECK(editor.CursorByteIndex()==11);
    editor.SetText("source");CHECK(editor.GetText()=="source"); // no synthetic newline on each save/reload
    ImGui::DestroyContext(context);ImGui::SetCurrentContext(previous);
}
void TestGlobalFileUndo() {
    namespace fs=std::filesystem;
    const auto directory=fs::current_path()/"build"/("global-undo-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    const auto original=directory/"original.asset",created=directory/"created.asset",renamed=directory/"renamed.asset";
    const std::string bytes("before\0\r\n",9);
    CHECK(AtomicFile::WriteBytes(original,bytes,true));
    EditorFileHistory::Journal journal;
    AtomicFile::SetChangeObserver([&](const fs::path& path){journal.Record(path);});
    CHECK(AtomicFile::WriteBytes(original,"drag 1",true));
    CHECK(AtomicFile::WriteBytes(original,"drag 2",true));
    CHECK(AtomicFile::WriteBytes(created,"new",true));
    AtomicFile::SetChangeObserver({});
    const auto before=journal.Changes();CHECK(before.size()==2);
    const auto after=EditorFileHistory::CaptureCurrent(before);
    CHECK(EditorFileHistory::Restore(before));
    CHECK(!fs::exists(created));CHECK(*EditorFileHistory::Capture(original).Bytes==bytes);
    CHECK(EditorFileHistory::Restore(after));
    CHECK(fs::exists(created));CHECK(*EditorFileHistory::Capture(original).Bytes=="drag 2");
    journal.Clear();journal.Record(original);journal.Record(renamed);fs::rename(original,renamed);
    const auto beforeRename=journal.Changes();CHECK(beforeRename.size()==2);
    const auto afterRename=EditorFileHistory::CaptureCurrent(beforeRename);
    CHECK(EditorFileHistory::Restore(beforeRename));CHECK(fs::exists(original) && !fs::exists(renamed));
    CHECK(EditorFileHistory::Restore(afterRename));CHECK(!fs::exists(original) && fs::exists(renamed));
    journal.Clear();journal.Record(renamed);fs::remove(renamed);
    CHECK(EditorFileHistory::Restore(journal.Changes()));CHECK(fs::exists(renamed));
    // Failed/no-op writes do not add history.
    journal.Clear();journal.Record(created);CHECK(journal.Changes().empty());
    fs::remove(created);fs::remove(renamed);fs::remove(directory);
}
void TestCurveAdapter() {
    Curve original;
    original.Keys={{0,-2,9,35},{0.12f,4,-7,0.25f},{0.65f,-1,12,-5},{1,0,3,7}};
    auto adapted=CurveEditor::ToImCurve(original);
    CHECK(CurveEditor::FromImCurve(adapted).ToJson()==original.ToJson());
    for (int i=-20;i<=120;++i) {
        float t=float(i)/100;
        CHECK(std::abs(adapted.Sample(t)-original.Evaluate(t))<0.00001f);
    }
    // Splitting a cubic must preserve the complete function, including steep tangents.
    CurveEditor::Insert(adapted,0.05f);
    CurveEditor::Insert(adapted,0.8f);
    CHECK(adapted.Points.size()==6);
    for (int i=0;i<=1000;++i) {
        float t=float(i)/1000;
        CHECK(std::abs(adapted.Sample(t)-original.Evaluate(t))<0.00001f);
    }
    CurveEditor::Insert(adapted,0.8f);
    CHECK(adapted.Points.size()==6);
    CHECK(!adapted.Points.back().HasControl());
    CHECK(adapted.Points.front().Points[2].X==kImCurveMax<float>);
    ImCurveCircularBuffer<int,3> history;
    for (int i=0;i<5;++i) history.Add(i);
    CHECK(history.Size()==3 && history[0]==2 && history[2]==4);
    history.Resize(2); history.Add(9);
    CHECK(history[0]==2 && history[2]==9);
    Curve partial=Curve::Line(0.2f,-2,0.8f,3);
    auto extended=CurveEditor::ToImCurve(partial);
    CurveEditor::Insert(extended,0.1f); CurveEditor::Insert(extended,0.9f);
    for (int i=0;i<=100;++i) CHECK(std::abs(extended.Sample(float(i)/100)-partial.Evaluate(float(i)/100))<0.00001f);
    Curve duplicates;
    duplicates.Keys={{0,1,0,0},{0,2,0,0},{0.5f,3,0,0},{0.5f,4,0,0},{1,0,0,0}};
    auto duplicateEditor=CurveEditor::ToImCurve(duplicates);
    CHECK(duplicateEditor.Sample(0)==duplicates.Evaluate(0));
    CHECK(duplicateEditor.Sample(0.5f)==duplicates.Evaluate(0.5f));
    CHECK(duplicateEditor.Sample(1)==duplicates.Evaluate(1));
}

void TestCurveEditorInteractions() {
    ImGuiContext* previous=ImGui::GetCurrentContext();
    ImGuiContext* context=ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize={900,700}; io.DeltaTime=1.0f/60;
    unsigned char* pixels=nullptr; int width=0,height=0;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    // The test uses the real ImGui input queue and draw path without a platform renderer.
    Curve original=Curve::EaseInOut();
    ImCurveEditor<float> editor(CurveEditor::ToImCurve(original));
    editor.Normalize=CurveEditor::Normalize; editor.Insert=CurveEditor::Insert;
    ImVec2 canvasMin{},canvasMax{};
    const auto frame=[&]() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({800,600});
        ImGui::Begin("Curve test",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
        editor.Draw("graph",{600,300});
        canvasMin=ImGui::GetItemRectMin(); canvasMax=ImGui::GetItemRectMax();
        ImGui::End(); ImGui::Render();
    };
    const auto project=[&](float x,float y) {
        auto v=editor.GetViewport(); float pad=io.FontDefault?io.FontDefault->LegacySize*0.5f:io.Fonts->Fonts[0]->LegacySize*0.5f;
        return ImVec2(canvasMin.x+pad+(x-v.Min.X)/v.GetWidth()*(canvasMax.x-canvasMin.x-2*pad),
                      canvasMin.y+pad+(1-(y-v.Min.Y)/v.GetHeight())*(canvasMax.y-canvasMin.y-2*pad));
    };
    frame(); frame();
    CHECK(CurveEditor::FromImCurve(editor.GetCurve()).ToJson()==original.ToJson());
    ImVec2 key=project(0,0);
    io.AddMousePosEvent(key.x,key.y); frame();
    io.AddMouseButtonEvent(0,true); frame();
    io.AddMousePosEvent(key.x+80,key.y-35); frame();
    CHECK(editor.IsDragging());
    CHECK(editor.GetCurve().Points[0].Points[0].X>0);
    CHECK(editor.GetCurve().Points[0].Points[0].Y>0);
    io.AddMouseButtonEvent(0,false); frame();
    CHECK(!editor.IsDragging() && editor.CanUndo());
    auto moved=editor.GetCurve();
    editor.Undo(); frame();
    CHECK(CurveEditor::FromImCurve(editor.GetCurve()).ToJson()==original.ToJson());
    CHECK(editor.CanRedo()); editor.Redo(); frame();
    CHECK(editor.GetCurve()==moved);
    // Pan/zoom affect the viewport only and do not create asset/history changes.
    auto beforePan=editor.GetViewport();
    io.AddMousePosEvent(canvasMin.x+300,canvasMin.y+80); frame();
    io.AddMouseWheelEvent(0,2); frame();
    CHECK(editor.GetViewport().GetWidth()<beforePan.GetWidth());
    CHECK(editor.GetCurve()==moved);
    io.AddMouseButtonEvent(1,true); frame();
    io.AddMousePosEvent(canvasMin.x+340,canvasMin.y+100); frame();
    io.AddMouseButtonEvent(1,false); frame();
    CHECK(editor.GetCurve()==moved);
    editor.SetCurve(CurveEditor::ToImCurve(original)); editor.Fit(); frame();
    // Multi-selection moves both keys with a shared time constraint.
    key=project(0,0); io.AddMousePosEvent(key.x,key.y); frame();
    io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame();
    auto second=project(1,1); io.AddMousePosEvent(second.x,second.y); frame();
    io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddMouseButtonEvent(0,true); frame();
    io.AddMouseButtonEvent(0,false); frame(); io.AddKeyEvent(ImGuiMod_Ctrl,false); frame();
    io.AddMousePosEvent(key.x,key.y); frame(); io.AddMouseButtonEvent(0,true); frame();
    io.AddMousePosEvent(key.x+120,key.y-30); frame(); io.AddMouseButtonEvent(0,false); frame();
    CHECK(editor.GetCurve().Points[0].Points[0].X==0); // endpoint at 1 clamps group to original time
    CHECK(editor.GetCurve().Points[0].Points[0].Y>0);
    CHECK(editor.GetCurve().Points[1].Points[0].Y>1);
    CHECK(std::abs(editor.GetCurve().Points[1].Points[0].Y-editor.GetCurve().Points[0].Points[0].Y-1)<0.00001f);
    // Delete all selected keys retains one valid constant key.
    io.AddKeyEvent(ImGuiKey_Delete,true); frame(); io.AddKeyEvent(ImGuiKey_Delete,false); frame();
    CHECK(editor.GetCurve().Points.size()==1);
    editor.SetCurve(CurveEditor::ToImCurve(original)); frame();
    CHECK(!editor.CanUndo() && !editor.CanRedo()); // externally reloaded asset wins over local history
    key=project(0,0); io.AddMousePosEvent(key.x,key.y); frame();
    io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame();
    auto handle=project(1.0f/3,0); io.AddMousePosEvent(handle.x,handle.y); frame();
    io.AddMouseButtonEvent(0,true); frame();
    io.AddMousePosEvent(handle.x,handle.y-30); frame();
    io.AddMouseButtonEvent(0,false); frame();
    CHECK(editor.GetCurve().Points[0].OutSlope>0);
    CHECK(editor.GetCurve().Points[0].InSlope==0 && editor.GetCurve().Points[1].InSlope==0);
    CHECK(ImGui::FindSettingsHandler("ImCurve")==nullptr);
    // Adapter commits one asset edit on release, never merely on redraw or navigation.
    Curve asset=original;
    bool captureCanvas=true;
    const auto assetFrame=[&]() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({800,600});
        ImGui::Begin("Asset curve test",nullptr,ImGuiWindowFlags_NoSavedSettings);
        bool committed=CurveEditor::DrawCanvas("asset curve",asset,{600,300});
        if (captureCanvas) { canvasMin=ImGui::GetItemRectMin(); canvasMax=ImGui::GetItemRectMax(); }
        ImGui::End(); ImGui::Render(); return committed;
    };
    CHECK(!assetFrame()); CHECK(!assetFrame());
    captureCanvas=false;
    const float pad=io.Fonts->Fonts[0]->LegacySize*0.5f;
    key={canvasMin.x+pad,canvasMin.y+pad+(1.12f/1.24f)*(canvasMax.y-canvasMin.y-2*pad)};
    io.AddMousePosEvent(key.x,key.y); CHECK(!assetFrame());
    CHECK(CurveEditor::OwnsKeyboard());
    io.AddMouseButtonEvent(0,true); CHECK(!assetFrame());
    io.AddMousePosEvent(key.x+60,key.y-25); CHECK(!assetFrame());
    CHECK(asset.Keys[0].Time>0 && asset.Keys[0].Value>0);
    io.AddMouseButtonEvent(0,false); CHECK(assetFrame());
    CHECK(!assetFrame());
    io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_Z,true); CHECK(assetFrame());
    CHECK(asset.ToJson()==original.ToJson());
    io.AddKeyEvent(ImGuiKey_Z,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); CHECK(!assetFrame());
    asset=Curve::Kick(); CHECK(!assetFrame());
    CHECK(asset.ToJson()==Curve::Kick().ToJson());
    ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous);
}
void TestCurveWheelCapture() {
    auto* previous=ImGui::GetCurrentContext(); auto* context=ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize={800,600}; io.DeltaTime=1.0f/60;
    unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    ImCurveEditor<float> editor(CurveEditor::ToImCurve(Curve::EaseInOut()));
    ImVec2 canvas{}; float scroll=0;
    const auto frame=[&]() {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({650,400});
        ImGui::Begin("Inspector wheel test",nullptr,ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("Curve"); editor.Draw("wheel curve",{500,180});
        canvas=ImGui::GetItemRectMin(); scroll=ImGui::GetScrollY();
        ImGui::Dummy({500,1000}); ImGui::End(); ImGui::Render();
    };
    frame(); frame();
    io.AddMousePosEvent(canvas.x+250,canvas.y+90); frame(); frame();
    auto before=editor.GetViewport(); float inspectorBefore=scroll;
    io.AddMouseWheelEvent(0,-1); frame(); frame();
    CHECK(editor.GetViewport().GetWidth()>before.GetWidth()); CHECK(scroll==inspectorBefore);
    // Moving below the graph returns wheel ownership to the Inspector.
    io.AddMousePosEvent(canvas.x+250,canvas.y+220); frame(); frame();
    before=editor.GetViewport(); io.AddMouseWheelEvent(0,-1); frame(); frame();
    CHECK(scroll>inspectorBefore); CHECK(editor.GetViewport().GetWidth()==before.GetWidth());
    ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous);
}
void TestUndoTriggerSkipsNavigation() {
    using UndoTrigger::Frame; using UndoTrigger::OpensSnapshot;
    Frame none; CHECK(!OpensSnapshot(none));
    // Navigation: no snapshot.
    Frame rmb; rmb.RightClicked=true; rmb.OverViewport=true; CHECK(!OpensSnapshot(rmb));
    Frame mmb; mmb.MiddleClicked=true; mmb.OverViewport=true; CHECK(!OpensSnapshot(mmb));
    Frame fly; fly.KeyPressed=true; fly.NavDragHeld=true; CHECK(!OpensSnapshot(fly));
    Frame orbit; orbit.LeftClicked=true; orbit.OverViewport=true; orbit.AltDown=true; CHECK(!OpensSnapshot(orbit));
    // Edits: still a snapshot.
    Frame click; click.LeftClicked=true; click.OverViewport=true; CHECK(OpensSnapshot(click));
    Frame handle=orbit; handle.GizmoHovered=true; CHECK(OpensSnapshot(handle));
    Frame altPanel; altPanel.LeftClicked=true; altPanel.AltDown=true; CHECK(OpensSnapshot(altPanel));
    Frame key; key.KeyPressed=true; CHECK(OpensSnapshot(key));
    Frame menu; menu.RightClicked=true; CHECK(OpensSnapshot(menu)); // a right-click on a panel
    Frame item; item.ItemActive=true; item.NavDragHeld=true; CHECK(OpensSnapshot(item));
    Frame side; side.OtherClicked=true; side.OverViewport=true; CHECK(OpensSnapshot(side));
}

// #14 - folders found from where files sit on disk stay in memory; only the folders settings.json
// listed and the ones made in the browser are written back, and they load again.
void TestAssetFoldersPersistOnlyUserFolders() {
    const std::string path=ProjectPaths::Resolve("settings.json");
    std::string original;
    { std::ifstream in(path,std::ios::binary); original.assign(std::istreambuf_iterator<char>(in),{}); }
    const std::vector<std::string> before=ProjectSettings::AssetFolders();
    ProjectSettings::SetAssetFolders({"UserA"});
    {
        AssetLibrary assets;
        assets.AddFolderPath("disk/found/here");
        CHECK(std::find(assets.Folders().begin(),assets.Folders().end(),"disk/found")!=assets.Folders().end());
        // Another case of a known folder reuses its spelling (the case-insensitive folder index).
        CHECK(assets.AddFolderPath("Disk/FOUND/other")=="disk/found/other");
        assets.CreateFolder("UserB");
        CHECK((ProjectSettings::AssetFolders()==std::vector<std::string>{"UserA","UserB"}));
        assets.RenameFolder("UserB","UserC");
        CHECK((ProjectSettings::AssetFolders()==std::vector<std::string>{"UserA","UserC"}));
        CHECK(assets.AddFolderPath("userc/x")=="UserC/x"); // the index follows a rename
        assets.DeleteFolder("disk/found/here"); // deleting a found folder still saves only the user's
        CHECK((ProjectSettings::AssetFolders()==std::vector<std::string>{"UserA","UserC"}));
        CHECK(assets.AddFolderPath("DISK/found/HERE")=="disk/found/HERE"); // ...and a delete
        ProjectSettings::Load();
        CHECK((ProjectSettings::AssetFolders()==std::vector<std::string>{"UserA","UserC"}));
        AssetLibrary reloaded;
        CHECK((reloaded.Folders()==std::vector<std::string>{"UserA","UserC"}));
        // A folder that is a real project directory (here "scenes") is listed but never saved:
        // undo snapshots re-create on-disk folders through CreateFolder.
        reloaded.CreateFolder("scenes");
        CHECK(std::find(reloaded.Folders().begin(),reloaded.Folders().end(),"scenes")!=reloaded.Folders().end());
        CHECK((ProjectSettings::AssetFolders()==std::vector<std::string>{"UserA","UserC"}));
    }
    {
        // A settings.json that already saved an on-disk folder is cleaned on the next folder change.
        ProjectSettings::SetAssetFolders({"UserA","scenes"});
        AssetLibrary polluted;
        polluted.CreateFolder("UserD");
        CHECK((ProjectSettings::AssetFolders()==std::vector<std::string>{"UserA","UserD"}));
    }
    // Memory first, file last: restoring the folders can save settings.json again with the
    // serializer's formatting, which left the tracked file modified after a test run.
    ProjectSettings::SetAssetFolders(before);
    { std::ofstream out(path,std::ios::binary|std::ios::trunc); out<<original; }
}
// Delete Folder takes a folder out of the browser and leaves its files on disk. An asset-index scan that lands
// afterwards (AddFolderPath per file directory) listed it again within a second, so the delete undid itself.
void TestDeletedDiskFolderStaysOut() {
    namespace fs=std::filesystem;
    const std::string top="unit-removed-folder";
    const std::string sub=top+"/Sub";
    std::error_code ec;
    fs::create_directories(fs::u8path(ProjectPaths::Resolve(sub)),ec);
    CHECK(!ec);
    auto listed=[](AssetLibrary& a,const std::string& f){ return std::find(a.Folders().begin(),a.Folders().end(),f)!=a.Folders().end(); };
    {
        AssetLibrary assets;
        CHECK(assets.AddFolderPath(sub)==sub);
        CHECK(listed(assets,top)&&listed(assets,sub));
        assets.DeleteFolderRecursive(top);
        CHECK(!listed(assets,top)&&!listed(assets,sub));
        CHECK(assets.IsRemovedDiskFolder(sub));
        // A scan that started before the delete lands after it: the files are still on disk.
        CHECK(assets.AddFolderPath(sub)==sub);
        CHECK(assets.AddFolderPath(top)==top);
        CHECK(assets.AddFolderPath("UNIT-Removed-Folder/sub")=="UNIT-Removed-Folder/sub"); // another case of it
        CHECK(!listed(assets,top)&&!listed(assets,sub));
        // Undo puts the folders back through CreateFolder: listed from their files again.
        assets.CreateFolder(top);
        CHECK(!assets.IsRemovedDiskFolder(sub));
        CHECK(assets.AddFolderPath(sub)==sub);
        CHECK(listed(assets,top)&&listed(assets,sub));
        // Deleted again, then the directory goes from disk: a new folder by that name is listed as usual.
        assets.DeleteFolderRecursive(top);
        CHECK(assets.IsRemovedDiskFolder(top));
        fs::remove_all(fs::u8path(ProjectPaths::Resolve(top)),ec);
        CHECK(!assets.IsRemovedDiskFolder(top));
        CHECK(assets.AddFolderPath(sub)==sub);
        CHECK(listed(assets,top)&&listed(assets,sub));
    }
    fs::create_directories(fs::u8path(ProjectPaths::Resolve(sub)),ec);
    {
        // Undo and redo go through the scene snapshot: the redo of Delete Folder keeps it out as well.
        World world;AssetLibrary assets;
        assets.AddFolderPath(sub);
        const std::string before=SceneSerializer::SaveToString(world,assets);
        assets.DeleteFolderRecursive(top);
        const std::string after=SceneSerializer::SaveToString(world,assets);
        CHECK(SceneSerializer::LoadFromString(world,assets,before)); // undo
        CHECK(listed(assets,top)&&listed(assets,sub)&&!assets.IsRemovedDiskFolder(sub));
        CHECK(SceneSerializer::LoadFromString(world,assets,after)); // redo
        CHECK(!listed(assets,top)&&!listed(assets,sub)&&assets.IsRemovedDiskFolder(sub));
        assets.AddFolderPath(sub); // a scan landing after the redo
        CHECK(!listed(assets,top)&&!listed(assets,sub));
    }
    fs::remove_all(fs::u8path(ProjectPaths::Resolve(top)),ec);
}
// #15 - an outfit landing (OutfitSystem::UpdatePending) makes the next frame's baseline the whole scene. Right
// before one, a change made without an undo step is journaled - but not the outfits' own pieces moving or
// coming and going (UpdateAttachments places rigid head wear on the frames after a landing), there or at Undo. The outfit
// assets are on Drive only, so this drives the check itself, with hand-made pieces and no wardrobe.
void TestOutfitLandingKeepsUnrecordedChange() {
    auto* previous=ImGui::GetCurrentContext();auto* context=ImGui::CreateContext();
    auto& io=ImGui::GetIO();io.DisplaySize={800,600};io.DeltaTime=1.0f/60;io.IniFilename=nullptr;
    unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    {
        World world;AssetLibrary assets;EditorLayer editor;
        const auto character=world.CreateEmptyEntity({0,0,0},{0,0,0},{1,1,1},"Character");
        world.Registry.emplace<CharacterOutfitComponent>(character).Wardrobe="no-such.wardrobe"; // no catalog: nothing rebuilds
        auto piece=[&](const char* slot) {
            const auto e=world.CreateEmptyEntity({0,0,0},{0,0,0},{1,1,1},slot);
            world.AttachChildRaw(e,character);
            world.Registry.emplace<OutfitPieceComponent>(e,OutfitPieceComponent{slot,std::string("Hat.fbx"),OutfitPieceHeadAttached});
            return e;
        };
        const auto hat=piece("Hat");
        const auto feather=world.CreateEmptyEntity({0,0,0},{0,0,0},{1,1,1},"Feather");world.AttachChildRaw(feather,hat);
        const auto glasses=piece("Glasses"); // written before the prop: its going renumbers the prop
        const auto prop=world.CreateEmptyEntity({1,2,3},{0,0,0},{1,1,1},"Prop");
        CHECK(OutfitSystem::LandingDue(world)); // a body not checked against its wardrobe yet
        OutfitSystem::UpdatePending(world,assets);
        CHECK(!OutfitSystem::LandingDue(world));

        auto frame=[&] {ImGui::NewFrame();EditorHistoryTestAccess::Begin(editor,world,assets);EditorHistoryTestAccess::Finish(editor,world);ImGui::Render();};
        frame();
        const auto baseline=EditorHistoryTestAccess::Current(editor);
        CHECK(!baseline.empty());

        // Outfit-derived changes only: no step.
        world.Registry.get<TransformComponent>(hat).Position={0,1.7f,0};world.Registry.get<TransformComponent>(feather).Position={0,0.2f,0};
        world.Registry.get<CharacterOutfitComponent>(character).Race="Other";
        world.DestroyEntityAndChildren(glasses);piece("Boots");
        CHECK(!SceneSerializer::SameSceneContent(SceneSerializer::SaveToString(world,assets),baseline));
        CHECK(SceneSerializer::SameSceneContentApartFromOutfits(SceneSerializer::SaveToString(world,assets),baseline));
        editor.JournalBeforeOutfitLanding(world);
        CHECK(EditorHistoryTestAccess::Count(editor)==0);CHECK(EditorHistoryTestAccess::Current(editor)==baseline);

        // A change outside the outfit, among them: its own step, which Undo takes back alone.
        world.Registry.get<TransformComponent>(prop).Position={4,5,6};
        world.Registry.get<CharacterOutfitComponent>(character).Locks="Hair";
        CHECK(!SceneSerializer::SameSceneContentApartFromOutfits(SceneSerializer::SaveToString(world,assets),baseline));
        const auto changed=SceneSerializer::SaveToString(world,assets);
        editor.JournalBeforeOutfitLanding(world);
        CHECK(EditorHistoryTestAccess::Count(editor)==1);CHECK(EditorHistoryTestAccess::TopLabel(editor)=="Unrecorded Change");
        CHECK(EditorHistoryTestAccess::Current(editor)==changed);
        editor.JournalBeforeOutfitLanding(world); // nothing new since
        CHECK(EditorHistoryTestAccess::Count(editor)==1);
        // The hat placed again since (UpdateAttachments, every frame): Undo folds that into the baseline
        // rather than journaling it, and takes back the step above.
        world.Registry.get<TransformComponent>(hat).Position={0,1.8f,0};
        EditorHistoryTestAccess::Undo(editor,world,assets);
        CHECK(EditorHistoryTestAccess::Count(editor)==0);
        CHECK(SceneSerializer::SaveToString(world,assets)==baseline);
        EditorHistoryTestAccess::Redo(editor,world,assets);
        CHECK(SceneSerializer::SameSceneContentApartFromOutfits(SceneSerializer::SaveToString(world,assets),changed)); // the hat where it was placed last
        for(auto [e,name,t]:world.Registry.view<NameComponent,TransformComponent>().each())
            if(name.Name=="Prop") CHECK(t.Position==glm::vec3(4,5,6)); // the registry was replaced: found by name
    }
    AtomicFile::SetChangeObserver({});ImGui::DestroyContext(context);ImGui::SetCurrentContext(previous);
}
}

// Unit tests for the editor UI. Add a function per test and list it below.

// Prefs v2 (the clean first launch): an older file's overlay/theme choices give way to the new
// defaults once, its other preferences survive, and the per-scene camera list stays capped.
void TestPrefsMigrationToCleanView() {
    EditorSettings& s = EditorSettings::Get();
    const EditorSettings saved = s;
    s.EditorThemeId = EditorSettings::kTartarusTheme;
    s.ShowColliders = true;
    s.SceneShowStats = true;
    s.LightGizmoSelectedOnly = false;
    s.SnapTranslation = 0.25f;
    s.LastScenePath = "scenes/Keep.json";
    s.GizmoSize = 0.15f; // the old default
    s.PrefsVersion = 1;
    CHECK(EditorSettings::MigratePrefs(s, 1)); // asks for the clean dock once
    CHECK(s.GizmoSize == EditorSettings{}.GizmoSize);
    CHECK(s.PrefsVersion == EditorSettings::kPrefsVersion);
    CHECK(s.EditorThemeId == EditorSettings::kGlassTheme);
    CHECK(!s.ShowColliders && s.CollidersSelectedOnly);
    CHECK(!s.SceneShowStats);
    CHECK(s.LightGizmoSelectedOnly && s.VolumeGizmosSelectedOnly);
    CHECK(s.EntityIconsMode == 1);
    CHECK(s.SnapTranslation == 0.25f);              // not a view default: kept
    CHECK(s.LastScenePath == "scenes/Keep.json");
    // v2 -> v3 only slims an untouched gizmo size; one you set stays.
    s.GizmoSize = 0.22f;
    CHECK(!EditorSettings::MigratePrefs(s, 2));
    CHECK(s.GizmoSize == 0.22f);
    // Already current: nothing changes and no layout reset.
    s.ShowColliders = true;
    CHECK(!EditorSettings::MigratePrefs(s, EditorSettings::kPrefsVersion));
    CHECK(s.ShowColliders);
    // Restore clean view resets the overlays but not the user's work settings.
    s.EntityIconsMode = 0;
    s.ResetViewDefaults();
    CHECK(!s.ShowColliders && s.EntityIconsMode == 1 && s.SnapTranslation == 0.25f);
    // Per-scene cameras: newest first, one entry per scene, capped.
    s.SceneCameras.clear();
    for (int i = 0; i < EditorSettings::kMaxSceneCameras + 5; ++i) {
        EditorSettings::SceneCameraPose p;
        p.ScenePath = "scenes/" + std::to_string(i) + ".json";
        p.X = (float)i;
        s.RememberSceneCamera(p);
    }
    CHECK((int)s.SceneCameras.size() == EditorSettings::kMaxSceneCameras);
    CHECK(s.SceneCameras.front().X == (float)(EditorSettings::kMaxSceneCameras + 4));
    EditorSettings::SceneCameraPose again;
    again.ScenePath = "scenes/10.json";
    again.X = 99.0f;
    s.RememberSceneCamera(again);
    CHECK((int)s.SceneCameras.size() == EditorSettings::kMaxSceneCameras);
    CHECK(s.FindSceneCamera("scenes/10.json") && s.FindSceneCamera("scenes/10.json")->X == 99.0f);
    CHECK(s.SceneCameras.front().ScenePath == "scenes/10.json");
    CHECK(!s.FindSceneCamera("scenes/0.json")); // the oldest fell off
    s = saved;
}

// The Asset Browser search syntax and the shared filter helpers (EditorSearch.h).
void TestEditorSearch() {
    using namespace EditorSearch;
    CHECK(MatchesFilter("", "anything"));
    CHECK(MatchesFilter("ROCK", "mossy_rock_01"));
    CHECK(!MatchesFilter("rocks", "rock"));
    CHECK(ParentFolderOf("Props/Guns/AK") == "Props/Guns" && ParentFolderOf("Props").empty());
    CHECK(LeafNameOf("Props/Guns/AK") == "AK" && LeafNameOf("Props") == "Props");
    const ParsedAssetSearch p = ParseAssetSearch("  coastal T:Model t:texture l:Hero  scene ");
    CHECK((p.nameTerms == std::vector<std::string>{"coastal", "scene"}));
    CHECK((p.typeTerms == std::vector<std::string>{"model", "texture"}));
    CHECK((p.labelTerms == std::vector<std::string>{"hero"}));
    CHECK(MatchesAssetSearch(p, "Coastal Scene 2", "texture", {"HERO"}));
    CHECK(!MatchesAssetSearch(p, "Coastal Scene 2", "sound", {"hero"}));   // type terms OR, this kind isn't one
    CHECK(!MatchesAssetSearch(p, "Coastal 2", "model", {"hero"}));         // name terms AND
    CHECK(!MatchesAssetSearch(p, "Coastal Scene", "model", {"heroes"}));   // labels match whole
    std::string f = "rock t:Model";
    CHECK(SearchHasToken(f, "t:Model") && !SearchHasToken(f, "t:Mod"));
    ToggleSearchToken(f, "t:Model");
    CHECK(f == "rock");
    ToggleSearchToken(f, "l:Hero");
    CHECK(f == "rock l:Hero");
}

// Moved from UnitTests.cpp (pure move).
namespace {

// --- UndoDelta chain: push three states, pop them back in reverse ---------------------------
struct TestEntry { UndoDelta::Patch Delta; int Tag = 0; };

// P-09: the auto-save writes the undo snapshot instead of reading the World. Stripped of its
// undo-only keys and given Save()'s library half, it must be Save()'s file byte for byte, with every
// optional key the snapshot can carry present (so a new undo-only key fails here, not in a user's file).
void TestAutoSaveFromUndoSnapshot() {
    World world;
    AssetLibrary assets;
    for (int i = 0; i < 4; ++i) world.CreateEmptyEntity({(float)i, 0.5f, -2.25f}, {}, {1, 1, 1}, "Autosave fixture " + std::to_string(i));
    assets.AddFolderPath("unit-autosave/Sub"); // listed (so the snapshot has assetFolders), never written to project settings
    assets.SetRemovedDiskFolders({"unit-autosave/Gone"}); // a Delete Folder mark (so the snapshot has removedDiskFolders)
    const std::filesystem::path dir = UnitTestSupport::TempDir("autosave-snapshot");
    const std::string fromSave = (dir / "save.json").string(), fromSnapshot = (dir / "snapshot.json").string();
    CHECK(SceneSerializer::Save(world, assets, fromSave));
    const std::string snapshot = SceneSerializer::SaveToString(world, assets);
    for (const char* key : {"\"hasAssetLibrarySnapshot\"", "\"assetFolders\"", "\"removedDiskFolders\"", "\"assetMeta\""})
        CHECK(snapshot.find(key) != std::string::npos);
    const std::function<bool()> write = SceneSerializer::PrepareSaveFromSnapshot(snapshot, assets, fromSnapshot);
    CHECK(write && write());
    const std::string want = UnitTestSupport::ReadAll(fromSave), got = UnitTestSupport::ReadAll(fromSnapshot);
    CHECK(!want.empty() && got == want);
    // A snapshot that won't parse writes nothing (and says so).
    std::error_code ec;
    std::filesystem::remove(fromSnapshot, ec);
    const std::function<bool()> broken = SceneSerializer::PrepareSaveFromSnapshot("{not json", assets, fromSnapshot);
    CHECK(broken && !broken() && !std::filesystem::exists(fromSnapshot));
    std::filesystem::remove_all(dir, ec);
}

void TestUndoDeltaChain() {
    const std::string s1 = R"({"entities":[{"name":"A","x":1}],"sky":1})";
    const std::string s2 = R"({"entities":[{"name":"A","x":2}],"sky":1})";
    const std::string s3 = R"({"entities":[{"name":"A","x":2},{"name":"B","x":5}],"sky":3})";

    std::vector<TestEntry> stack;
    std::string base;
    UndoDelta::Push(stack, base, TestEntry{{}, 1}, s1);
    UndoDelta::Push(stack, base, TestEntry{{}, 2}, s2);
    UndoDelta::Push(stack, base, TestEntry{{}, 3}, s3);
    CHECK(stack.size() == 3);
    CHECK(stack.back().Delta.empty()); // the top is held in full

    auto same = [](const std::string& a, const std::string& b) {
        return nlohmann::json::parse(a) == nlohmann::json::parse(b);
    };
    TestEntry e;
    std::string full;
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Ok);
    CHECK(e.Tag == 3 && same(full, s3));
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Ok);
    CHECK(e.Tag == 2 && same(full, s2));
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Ok);
    CHECK(e.Tag == 1 && same(full, s1));
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Empty);

    // X-33: Pop can hand back the full state parsed too (undo / redo load it without parsing again),
    // both while re-anchoring on the entry underneath and for the last entry.
    UndoDelta::Push(stack, base, TestEntry{{}, 1}, s1);
    UndoDelta::Push(stack, base, TestEntry{{}, 2}, s2);
    nlohmann::json parsed;
    CHECK(UndoDelta::Pop(stack, base, e, full, &parsed) == UndoDelta::PopResult::Ok);
    CHECK(e.Tag == 2 && same(full, s2) && parsed == nlohmann::json::parse(s2) && same(base, s1));
    CHECK(UndoDelta::Pop(stack, base, e, full, &parsed) == UndoDelta::PopResult::Ok);
    CHECK(e.Tag == 1 && parsed == nlohmann::json::parse(s1) && base.empty());
    // A top that won't parse comes back as a discarded value (the caller parses the text itself).
    stack.push_back(TestEntry{{}, 9});
    base = "{broken";
    CHECK(UndoDelta::Pop(stack, base, e, full, &parsed) == UndoDelta::PopResult::Ok);
    CHECK(parsed.is_discarded() && full == "{broken");

    // Malformed input never throws out of the helpers.
    std::string out;
    CHECK(!UndoDelta::ApplyPatch(s1, "{not json", out));
    CHECK(!UndoDelta::ApplyPatch(s1, "", out));
    CHECK(UndoDelta::MakePatch("{broken", s1).empty());
}

} // namespace

// The editor caches' use-ordered map (LruMap.h): Find counts as a use, Peek doesn't, trims drop the oldest.
void TestLruMap() {
    LruMap<int> m;
    m.Insert("a", 1); m.Insert("b", 2); m.Insert("c", 3); // oldest a, b, c newest
    CHECK(m.Size() == 3 && m.Find("missing") == nullptr);
    CHECK(m.Find("a") && *m.Find("a") == 1);             // a is now the newest: b, c, a
    CHECK(m.Peek("b") && *m.Peek("b") == 2);             // no reorder
    std::vector<int> dropped;
    m.TrimTo(2, [&](int& v) { dropped.push_back(v); });
    CHECK((dropped == std::vector<int>{2}) && !m.Peek("b") && m.Size() == 2);
    m.Insert("c", 30);                                    // replace keeps one entry and makes it newest: a, c
    CHECK(m.Size() == 2 && *m.Peek("c") == 30);
    m.TrimTo(1);
    CHECK(!m.Peek("a") && m.Peek("c"));
    int erased = 0;
    CHECK(m.Erase("c", [&](int& v) { erased = v; }) && erased == 30 && !m.Erase("c"));
    m.Insert("x", 7); m.Insert("y", 8);
    std::vector<int> order;
    m.ForEach([&](int& v) { order.push_back(v); });
    CHECK((order == std::vector<int>{8, 7}));             // newest first
    m.Clear();
    CHECK(m.Size() == 0 && !m.Peek("x"));
    m.Insert("x", 9);                                     // usable after Clear
    CHECK(m.Size() == 1 && *m.Find("x") == 9);
}

// The Hierarchy's cached creation order (kept between frames, re-sorted only when the
// (OrderComponent, entity) pairs change) must still follow every create, destroy, slot reuse,
// reorder and reparent, with no ImGui frame in between.
// A parent's Children list follows the Hierarchy's order (OrderComponent): SetParent inserts in place, and a drag
// reorder re-sorts it and bumps HierarchyVersion, so a LOD Group's levels are the rows in the order shown (they kept
// the attach order until a save and reload), and caches keyed on the version rebuild.
void TestHierarchyChildOrder() {
    using X = EditorHistoryTestAccess;
    World world; EditorLayer editor;
    auto make = [&](const char* n) { return world.CreateEmptyEntity({}, {}, {1,1,1}, n); };
    const entt::entity group = make("Group");
    const entt::entity a = make("A"), b = make("B"), c = make("C");
    auto kids = [&] { return world.Registry.get<HierarchyComponent>(group).Children; };
    // Attached out of creation order: still listed in it.
    CHECK(world.SetParent(c, group) && world.SetParent(a, group) && world.SetParent(b, group));
    CHECK((kids() == std::vector<entt::entity>{a, b, c}));
    world.AttachChildRaw(make("D"), group);
    CHECK(kids().size() == 4 && kids().back() != a);
    // The Hierarchy's drag reorder: C above A.
    const std::uint64_t before = world.HierarchyVersion();
    X::Reorder(editor, world, {c}, a, false);
    CHECK((X::Siblings(editor, world, group) == std::vector<entt::entity>{c, a, b, kids().back()}));
    const std::vector<entt::entity> now = kids();
    CHECK((std::vector<entt::entity>(now.begin(), now.begin() + 3) == std::vector<entt::entity>{c, a, b}));
    CHECK(world.HierarchyVersion() != before);
    // Already in order: no bump.
    const std::uint64_t after = world.HierarchyVersion();
    world.SortChildrenByOrder(group);
    CHECK(world.HierarchyVersion() == after);
    // A LOD Group's LOD 0 is now C: a renderer only on C shows up close.
    world.Registry.emplace<LODGroupComponent>(group).Size = 2.0f;
    for (entt::entity e : kids()) world.Registry.emplace<RenderableComponent>(e);
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
    CHECK(world.ApplyLod(glm::vec3(0.0f, 0.0f, 1.0f), proj) == 1);
    CHECK(!world.IsLodCulled(c) && world.IsLodCulled(a) && world.IsLodCulled(b));
    // A raw entity (no OrderComponent) attached later goes last, not to LOD 0.
    const entt::entity raw = world.Registry.create();
    world.AttachChildRaw(raw, group);
    CHECK(kids().back() == raw && kids().front() == c);
}

void TestHierarchyCreationOrderCache() {
    using X = EditorHistoryTestAccess;
    World world; EditorLayer editor;
    auto make = [&](const char* n) { return world.CreateEmptyEntity({}, {}, {1,1,1}, n); };
    const entt::entity a = make("A"), b = make("B"), c = make("C");
    CHECK((X::Order(editor, world) == std::vector<entt::entity>{a, b, c}));
    CHECK(X::Ordinal(editor, world, a) == 1 && X::Ordinal(editor, world, c) == 3);
    // Reordering by OrderComponent alone (no entity added or removed) re-sorts.
    world.Registry.get<OrderComponent>(a).Value = world.Registry.get<OrderComponent>(c).Value + 1;
    CHECK((X::Order(editor, world) == std::vector<entt::entity>{b, c, a}));
    CHECK(X::Ordinal(editor, world, a) == 3 && X::Ordinal(editor, world, b) == 1);
    // Destroy then create: the new entity may reuse b's slot; the stale handle must not match.
    world.Registry.destroy(b);
    const entt::entity d = make("D");
    CHECK(X::Ordinal(editor, world, b) == 0);
    CHECK(X::Order(editor, world).size() == 3 && X::Order(editor, world).back() == d);
    CHECK(X::Ordinal(editor, world, d) == 3);
    // Scene roots come out of the cache in creation order; a child leaves the roots and
    // HierarchySiblingsInOrder sorts children by OrderComponent.
    CHECK(world.SetParent(d, c));
    const entt::entity e = make("E");
    CHECK(world.SetParent(e, c));
    CHECK((X::Siblings(editor, world, entt::null) == std::vector<entt::entity>{c, a}));
    CHECK((X::Siblings(editor, world, c) == std::vector<entt::entity>{d, e}));
    world.Registry.get<OrderComponent>(d).Value = world.Registry.get<OrderComponent>(e).Value + 1;
    CHECK((X::Siblings(editor, world, c) == std::vector<entt::entity>{e, d}));
    // Unchanged input: the same list comes back, cheaply (one linear compare, no sort).
    std::vector<entt::entity> big;
    for (int i = 0; i < 20000; ++i) big.push_back(make("Bulk"));
    const auto& first = X::Order(editor, world);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) CHECK(&X::Order(editor, world) == &first);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
    std::printf("  hierarchy order, 20k unchanged: %.3f ms/call\n", ms);
    CHECK(X::Ordinal(editor, world, big.back()) == (int)X::Order(editor, world).size());
}

// Rebuild & Restart: the state file round-trips, stale / malformed files are ignored, and a
// selection comes back by order + name (a renamed or missing entity drops out).
// P-24: the Inspector's per-frame exists / mtime checks hit the disk at most once per kMaxAge per path.
void TestEditorFileStatCache() {
    namespace FS = EditorFileStat;
    const auto dir = UnitTestSupport::TempDir("editor_file_stat");
    const auto file = dir / "a.txt";
    std::error_code ec;
    std::filesystem::remove(file, ec);
    FS::Clear();
    const auto t0 = FS::Clock::now();
    const std::uint64_t r0 = FS::DiskReads();
    CHECK(!FS::Get(file, t0).Exists);
    for (int i = 0; i < 100; ++i) CHECK(!FS::Get(file, t0 + std::chrono::milliseconds(i)).Exists); // 100 "frames"
    CHECK(FS::DiskReads() - r0 == 1);
    { std::ofstream(file) << "x"; }
    CHECK(!FS::Get(file, t0 + std::chrono::milliseconds(100)).Exists);            // still cached
    CHECK(FS::Get(file, t0 + FS::kMaxAge + std::chrono::milliseconds(1)).Exists); // re-read once it ages out
    CHECK(FS::DiskReads() - r0 == 2);
    const auto stamp = FS::Get(file, t0 + FS::kMaxAge + std::chrono::milliseconds(2)).WriteTime;
    CHECK(stamp == std::filesystem::last_write_time(file, ec));
    std::filesystem::remove(file, ec);
    CHECK(!FS::Refresh(file, t0 + FS::kMaxAge + std::chrono::milliseconds(3)).Exists); // own write: fresh at once
    CHECK(FS::DiskReads() - r0 == 3);
    CHECK(!FS::Get(std::filesystem::path(), t0).Exists);
    CHECK(FS::DiskReads() - r0 == 3); // an empty path never touches the disk

    // Micro-timing for the PR (opt in: TARTARUS_MICROBENCH=1): the raw calls the Inspector and the File menu
    // made every frame, against a cached Get.
    if (Env::Flag(Env::Var::MicroBench)) {
        { std::ofstream(file) << "x"; }
        const auto other = dir / "b.txt";
        { std::ofstream(other) << "y"; }
        using C = std::chrono::steady_clock;
        static constexpr int kN = 2000; // static: the lambda below reads it without a capture (MSVC)
        auto us = [](C::duration d) { return std::chrono::duration<double, std::micro>(d).count() / kN; };
        bool sink = false;
        auto t0 = C::now();
        for (int i = 0; i < kN; ++i) sink ^= std::filesystem::exists(file, ec);
        const double exUs = us(C::now() - t0);
        t0 = C::now();
        for (int i = 0; i < kN; ++i) sink ^= std::filesystem::last_write_time(file, ec) == std::filesystem::file_time_type{};
        const double mtUs = us(C::now() - t0);
        t0 = C::now();
        for (int i = 0; i < kN; ++i) sink ^= std::filesystem::equivalent(file, other, ec);
        const double eqUs = us(C::now() - t0);
        FS::Clear();
        t0 = C::now();
        for (int i = 0; i < kN; ++i) sink ^= FS::Get(file).Exists;
        const double cachedUs = us(C::now() - t0);
        std::printf("[MicroBench] file stat: exists %.2f us, last_write_time %.2f us, equivalent %.2f us, cached Get %.3f us (sink=%d)\n",
                    exUs, mtUs, eqUs, cachedUs, (int)sink);
        std::filesystem::remove(file, ec);
        std::filesystem::remove(other, ec);
    }
    FS::Clear();
}

void TestRestartStateRoundTrip() {
    namespace RS = RestartState;
    const auto file = UnitTestSupport::TempDir("restart_state") / "restart_state.json";
    RS::State s;
    s.ScenePath = "assets/Scenes/Hallway.json";
    s.RestoreRecovery = true;
    s.WasPlaying = true;
    s.Selection = {{7, "Door"}, {12, ""}};
    CHECK(RS::Save(file, s));
    const std::int64_t now = RS::NowUnix();
    auto back = RS::Load(file, now);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->ScenePath == s.ScenePath);
        CHECK(back->RestoreRecovery && back->WasPlaying);
        CHECK(back->Selection.size() == 2 && back->Selection[0].Order == 7 && back->Selection[0].Name == "Door");
        CHECK(back->Selection[1].Order == 12 && back->Selection[1].Name.empty());
    }
    std::string why;
    CHECK(!RS::Load(file, now + RS::kMaxAgeSeconds + 5, &why) && why.find("stale") != std::string::npos);
    { std::ofstream(file, std::ios::trunc) << "{ not json"; }
    CHECK(!RS::Load(file, now, &why) && why.find("malformed") != std::string::npos);
    std::filesystem::remove(file);
    CHECK(!RS::Load(file, now, &why) && why.find("no restart") != std::string::npos);

    World world;
    const entt::entity a = world.CreateEmptyEntity({}, {}, glm::vec3(1.0f), "Alpha");
    const entt::entity b = world.CreateEmptyEntity({}, {}, glm::vec3(1.0f), "Beta");
    const entt::entity c = world.CreateEmptyEntity({}, {}, glm::vec3(1.0f), "Gamma");
    const auto refs = RS::RefsFor(world, {c, a, entt::null});
    CHECK(refs.size() == 2 && refs[0].Name == "Gamma" && refs[1].Name == "Alpha");
    CHECK((RS::Resolve(world, refs) == std::vector<entt::entity>{c, a}));
    world.Registry.get<NameComponent>(c).Name = "Renamed";
    CHECK((RS::Resolve(world, refs) == std::vector<entt::entity>{a}));
    world.Registry.destroy(a);
    CHECK(RS::Resolve(world, refs).empty());
    (void)b;
}

// The relaunch command line: old hand-over flags dropped (both spellings), new ones last, Windows quoting.
void TestRestartRelaunchArgs() {
    namespace ER = EditorRestart;
    const std::vector<std::wstring> in = {L"--scene", L"a b.json", L"--restore-state", L"old.json", L"--wait-pid=12", L"--x"};
    const std::vector<std::wstring> out = ER::RelaunchArgs(in, L"C:/u/restart_state_9.json", 9);
    const std::vector<std::wstring> want = {L"--scene", L"a b.json", L"--x", L"--restore-state", L"C:/u/restart_state_9.json",
                                            L"--wait-pid", L"9"};
    CHECK(out == want);
    CHECK(ER::QuoteArg(L"plain") == L"plain");
    CHECK(ER::QuoteArg(L"") == L"\"\"");
    CHECK(ER::QuoteArg(L"a b") == L"\"a b\"");
    CHECK(ER::QuoteArg(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
    CHECK(ER::QuoteArg(L"C:\\dir with space\\") == L"\"C:\\dir with space\\\\\"");
}

void RegisterEditorTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("Hierarchy creation order cache follows create, destroy, reorder and reparent",TestHierarchyCreationOrderCache);
    tests.emplace_back("Hierarchy child order: Children follow OrderComponent, a reorder bumps the version and moves LOD levels",TestHierarchyChildOrder);
    tests.emplace_back("Editor navigation undo avoids scene snapshots and preserves transactions",TestEditorNavigationUndo);
    tests.emplace_back("Prefab toolbar replaces Play and returns from hidden Scene tab",TestPrefabToolbarReturn);
    tests.emplace_back("Script IDE text editing, completion undo and source preservation",TestScriptIDETextEditing);
    tests.emplace_back("Global file undo, coalescing, creation, deletion and rename",TestGlobalFileUndo);
    tests.emplace_back("ImCurveAdapter",TestCurveAdapter);
    tests.emplace_back("ImCurveEditorInteractions",TestCurveEditorInteractions);
    tests.emplace_back("CurveWheelCapture",TestCurveWheelCapture);
    tests.emplace_back("UndoTriggerSkipsNavigation",TestUndoTriggerSkipsNavigation);
    tests.emplace_back("Asset folders: only the user's are saved to project settings",TestAssetFoldersPersistOnlyUserFolders);
    tests.emplace_back("Asset folders: a folder deleted in the browser stays out while its files are on disk",TestDeletedDiskFolderStaysOut);
    tests.emplace_back("Outfit landing: an unrecorded change before it stays its own undo step",TestOutfitLandingKeepsUnrecordedChange);
    tests.emplace_back("Editor prefs v2: clean view migration, restore defaults, per-scene cameras",TestPrefsMigrationToCleanView);
    tests.emplace_back("Editor search: filters, t: / l: syntax, search tokens",TestEditorSearch);
    tests.push_back({"UndoDeltaChain", TestUndoDeltaChain});
    tests.emplace_back("Auto-save from the undo snapshot writes what Save writes",TestAutoSaveFromUndoSnapshot);
    tests.emplace_back("Editor LruMap: use order, peek, trim, erase",TestLruMap);
    tests.emplace_back("Editor file stat cache: one disk read per path per half second, Refresh after own writes",TestEditorFileStatCache);
    tests.emplace_back("Rebuild & Restart state file: round trip, stale, malformed, selection by order + name",TestRestartStateRoundTrip);
    tests.emplace_back("Rebuild & Restart relaunch arguments and quoting",TestRestartRelaunchArgs);
}
