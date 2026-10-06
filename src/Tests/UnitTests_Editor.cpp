#include "UnitTestSupport.h"
#include "ImCurveAdapter.h"
#include "CurveEditor.h"
#include "EditorFileHistory.h"
#include "../../extern/ImGuiColorTextEdit/TextEditor.h"
#include "../../extern/imcurve/imcurve_editor.hpp"
#include <imgui_internal.h>
#include <cmath>
#include <chrono>

namespace {
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
}

// Unit tests for the editor UI. Add a function per test and list it below.

void RegisterEditorTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("Script IDE text editing, completion undo and source preservation",TestScriptIDETextEditing);
    tests.emplace_back("Global file undo, coalescing, creation, deletion and rename",TestGlobalFileUndo);
    tests.emplace_back("ImCurveAdapter",TestCurveAdapter);
    tests.emplace_back("ImCurveEditorInteractions",TestCurveEditorInteractions);
    tests.emplace_back("CurveWheelCapture",TestCurveWheelCapture);
}
