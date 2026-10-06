#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "imcurve.hpp"


template<typename T, size_t N>
class ImCurveCircularBuffer
{
    static_assert(N > 0, "History must have positive capacity");
public:
    ImCurveCircularBuffer()
        : Data{}
        , First{0}
        , Count{0}
    {
    }

    void Add(const T& value)
    {
        Data[(First + Count) % N] = value;
        if (Count < N)
        {
            Count++;
        }
        else
        {
            First = (First + 1) % N;
        }
    }

    void Resize(size_t size)
    {
        assert(size <= Count);
        Count = size;
        if (!Count)
        {
            First = 0;
        }
    }

    T& operator[](size_t index)
    {
        return Data[(First + index) % N];
    }

    const T& operator[](size_t index) const
    {
        return Data[(First + index) % N];
    }

    size_t Size() const
    {
        return Count;
    }

    void Clear()
    {
        Resize(0);
    }

private:
    std::array<T, N> Data;
    size_t First;
    size_t Count;
};

template<typename T>
struct ImCurveEditor
{
private:
    float kPadding = 8.0f;

    struct Reference
    {
        Reference()
            : Index{-1}
            , Type{ImCurvePointType_Count}
        {
        }

        Reference(int index, ImCurvePointType type)
            : Index{index}
            , Type{type}
        {
        }

        explicit operator bool() const
        {
            return Index != -1;
        }

        bool operator==(const Reference& other) const
        {
            return Index == other.Index && Type == other.Type;
        }

        int Index;
        ImCurvePointType Type;
    };

    enum EditType
    {
        EditType_None,
        EditType_MoveCanvas,
        EditType_MovePoints,
        EditType_RectSelect,
    };

public:
    ImCurveEditor(ImCurve<T> curve = {})
        : History{}
        , HistoryIndex{0}
        , References{}
        , Viewport{}
        , EditStart{}
        , Edit{EditType_None}
        , IsEditStarted{false}
        , IsLoaded{false}
    {
        assert(curve.Points.empty() || !curve.Points.back().HasControl());
        History.Add(std::move(curve));
    }

    const ImCurve<T>& GetCurve() const
    {
        return History[HistoryIndex];
    }

    // Tartarus adapter: asset data is authoritative; no curve data goes into imgui.ini.
    ImU32 Color = 0;
    T TimeMin = T{}, TimeMax = T{1};
    bool KeyboardHistory = true;
    bool Snap = false, LinkTangents = false;
    T TimeSnap = T{0.01}, ValueSnap = T{0.01};
    std::function<void(ImCurve<T>&)> Normalize;
    std::function<void(ImCurve<T>&, T)> Insert;
    bool IsDragging() const { return Edit == EditType_MovePoints; }
    int Selected() const { return References.empty() ? -1 : References.front().Index; }
    std::vector<int> SelectedKeys() const {
        std::vector<int> result;
        for (const auto& ref:References) if (std::find(result.begin(),result.end(),ref.Index)==result.end()) result.push_back(ref.Index);
        std::sort(result.begin(),result.end()); return result;
    }
    void SelectKeys(const std::vector<int>& keys) {
        References.clear();
        for (int i:keys) if (i>=0 && i<(int)GetCurve().Points.size()) References.push_back({i,ImCurvePointType_Start});
    }
    void SetViewport(const ImCurveRect<T>& view) { Viewport=view; IsLoaded=true; }
    const ImCurveRect<T>& GetViewport() const { return Viewport; }
    bool CanUndo() const { return HistoryIndex > 0; }
    bool CanRedo() const { return HistoryIndex+1 < History.Size(); }
    void Undo() { if (CanUndo()) { --HistoryIndex; References.clear(); } }
    void Redo() { if (CanRedo()) { ++HistoryIndex; References.clear(); } }
    void Fit() { IsLoaded = false; }
    void SetCurve(const ImCurve<T>& curve) {
        History.Clear(); History.Add(curve); HistoryIndex = 0;
        References.clear(); Edit = EditType_None; IsEditStarted = false;
    }
    void UpdateCurve(const ImCurve<T>& curve, bool newEntry) {
        if (newEntry && curve != GetCurve()) {
            History.Resize(HistoryIndex+1); History.Add(curve); HistoryIndex=History.Size()-1;
        } else History[HistoryIndex]=curve;
    }

    void Draw(const char* label, ImVec2 size = {-1.0f, -1.0f}, const std::vector<T>& highlightedPoints = {})
    {
        kPadding=ImGui::GetFontSize()*0.5f;
        if (!IsLoaded)
        {
            IsLoaded = true;
            const ImCurve<T>& curve = GetCurve();
            Viewport = {};
            if (!curve.Points.empty())
            {
                Viewport.Min = curve.Points.front().Points[ImCurvePointType_Start];
                Viewport.Max = Viewport.Min;
                for (const ImCurvePoint<T>& point : curve.Points)
                {
                    Viewport.Expand(point.Points[ImCurvePointType_Start]);
                    for (int type=1; type<ImCurvePointType_Count; ++type)
                        if (point.Points[type].X != kImCurveMax<T>) Viewport.Expand(point.Points[type]);
                }
            }
            if (Viewport.GetWidth() == T{})
            {
                Viewport.Max.X += T{1};
            }
            if (Viewport.GetHeight() == T{})
            {
                Viewport.Min.Y -= T{0.5};
                Viewport.Max.Y += T{0.5};
            }
            Viewport.Min.X = std::min(Viewport.Min.X, TimeMin);
            Viewport.Max.X = std::max(Viewport.Max.X, TimeMax);
            T pad = std::max(Viewport.GetHeight() * T{0.12}, T{0.001});
            Viewport.Min.Y -= pad; Viewport.Max.Y += pad;
        }
        if (size.x < 0.0f || size.y < 0.0f)
        {
            size = {ImGui::GetContentRegionAvail().x, 72.0f};
        }
        ImGui::PushID(label);
        bool editor = true;
        ImVec2 canvasMin = ImGui::GetCursorScreenPos();
        ImVec2 canvasMax{canvasMin.x + size.x, canvasMin.y + size.y};
        ImVec2 plotSize{std::max(size.x - kPadding * 2.0f, 1.0f), std::max(size.y - kPadding * 2.0f, 1.0f)};
        ImGui::InvisibleButton("Canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
        bool isHovered = ImGui::IsItemHovered();
        bool isActive = ImGui::IsItemActive();
        ImGuiIO& io = ImGui::GetIO();
        Reference hoveredReference;
        if (editor && isHovered)
        {
            float hoveredPointDistanceSquared = ImGui::GetFontSize()*ImGui::GetFontSize()*0.25f;
            for (int point = 0; point < GetCurve().Points.size(); point++)
            {
                for (int i = 0; i < ImCurvePointType_Count; i++)
                {
                    ImCurvePointType type = ImCurvePointType(i);
                    if (GetCurve().Points[point].Points[type].X == kImCurveMax<T>) continue;
                    if (type!=ImCurvePointType_Start &&
                        std::find(References.begin(),References.end(),Reference{point,ImCurvePointType_Start})==References.end() &&
                        std::find(References.begin(),References.end(),Reference{point,type})==References.end()) continue;
                    ImVec2 position = Project(GetCurve().Points[point].Points[type], canvasMin, plotSize);
                    float x = position.x - io.MousePos.x;
                    float y = position.y - io.MousePos.y;
                    float distanceSquared = x * x + y * y;
                    if (distanceSquared <= hoveredPointDistanceSquared)
                    {
                        hoveredPointDistanceSquared = distanceSquared;
                        hoveredReference = {point, ImCurvePointType(i)};
                    }
                }
            }
        }
        if (editor)
        {
            ImCurve<T> curve = GetCurve();
            if (isHovered && io.MouseWheel != 0.0f)
            {
                ImCurveVec2<T> cursor = Unproject(io.MousePos, canvasMin, plotSize);
                T zoom = std::pow(1.1f, -std::clamp(io.MouseWheel, -10.0f, 10.0f));
                if ((Viewport.GetWidth() < T{0.00001} || Viewport.GetHeight() < T{0.00001}) && zoom < T{1}) zoom = T{1};
                if ((Viewport.GetWidth() > T{1000000} || Viewport.GetHeight() > T{1000000}) && zoom > T{1}) zoom = T{1};
                Viewport.Min.X = cursor.X + (Viewport.Min.X - cursor.X) * zoom;
                Viewport.Max.X = cursor.X + (Viewport.Max.X - cursor.X) * zoom;
                Viewport.Min.Y = cursor.Y + (Viewport.Min.Y - cursor.Y) * zoom;
                Viewport.Max.Y = cursor.Y + (Viewport.Max.Y - cursor.Y) * zoom;
            }
            if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                if (hoveredReference)
                {
                    References = {hoveredReference};
                    ImGui::OpenPopup("Point");
                }
                else
                {
                    Edit = EditType_MoveCanvas;
                    IsEditStarted = true;
                }
            }
            if (isHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !hoveredReference)
            {
                IsEditStarted = true;
                ImCurvePoint<T> point;
                point.Points[ImCurvePointType_Start] = Unproject(io.MousePos, canvasMin, plotSize);
                if (Insert) Insert(curve, std::clamp(point.Points[ImCurvePointType_Start].X, TimeMin, TimeMax));
                References.clear();
                Edit = EditType_None;
            }
            else if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                if (hoveredReference)
                {
                    auto selectedPoint = std::find(References.begin(), References.end(), hoveredReference);
                    if (io.KeyCtrl)
                    {
                        if (selectedPoint == References.end())
                        {
                            References.push_back(hoveredReference);
                        }
                        else
                        {
                            References.erase(selectedPoint);
                        }
                    }
                    else
                    {
                        if (selectedPoint == References.end())
                        {
                            References = {hoveredReference};
                        }
                        Edit = EditType_MovePoints;
                        IsEditStarted = true;
                    }
                }
                else
                {
                    if (!io.KeyCtrl)
                    {
                        References.clear();
                    }
                    Edit = EditType_RectSelect;
                    IsEditStarted = true;
                    EditStart = io.MousePos;
                }
            }
            ImCurveVec2<T> mouseDelta{io.MouseDelta.x / plotSize.x * Viewport.GetWidth(), -io.MouseDelta.y / plotSize.y * Viewport.GetHeight()};
            switch (Edit)
            {
            case EditType_MovePoints:
            {
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    if (Snap) for (const auto& ref:References) if (ref.Type==ImCurvePointType_Start) {
                        auto& point=curve.Points[ref.Index];
                        T lo=ref.Index>0?curve.Points[ref.Index-1].Points[0].X+T{0.0001}:TimeMin;
                        T hi=ref.Index+1<curve.Points.size()?curve.Points[ref.Index+1].Points[0].X-T{0.0001}:TimeMax;
                        if (TimeSnap>T{}) point.Points[0].X=std::clamp(std::round(point.Points[0].X/TimeSnap)*TimeSnap,lo,std::max(lo,hi));
                        if (ValueSnap>T{}) point.Points[0].Y=std::round(point.Points[0].Y/ValueSnap)*ValueSnap;
                    }
                    Edit = EditType_None;
                    break;
                }
                // Clamp the whole group against unselected neighbours: keys cannot cross.
                T deltaX = mouseDelta.X;
                const auto selected = [&](int index) {
                    return std::find(References.begin(), References.end(), Reference{index, ImCurvePointType_Start}) != References.end();
                };
                for (const Reference& ref : References) if (ref.Type == ImCurvePointType_Start) {
                    T x = curve.Points[ref.Index].Points[0].X;
                    T lo = ref.Index > 0 && !selected(ref.Index-1) ? curve.Points[ref.Index-1].Points[0].X + T{0.0001} : TimeMin;
                    T hi = ref.Index+1 < curve.Points.size() && !selected(ref.Index+1) ? curve.Points[ref.Index+1].Points[0].X - T{0.0001} : TimeMax;
                    deltaX = std::clamp(deltaX, std::min(T{}, lo-x), std::max(T{}, hi-x));
                }
                for (const Reference& ref : References) {
                    auto& point = curve.Points[ref.Index];
                    if (ref.Type == ImCurvePointType_Start) {
                        point.Points[0].X += deltaX; point.Points[0].Y += mouseDelta.Y;
                    } else if (!selected(ref.Index)) {
                        T dx = point.Points[ref.Type].X - point.Points[0].X;
                        if (std::abs(dx) > T{0.000001}) {
                            T& slope = ref.Type == ImCurvePointType_InControl ? point.InSlope : point.OutSlope;
                            slope += mouseDelta.Y / dx;
                            if (LinkTangents) point.InSlope=point.OutSlope=slope;
                        }
                    }
                }
                break;
            }
            case EditType_RectSelect:
            {
                References.clear();
                float x = io.MousePos.x - EditStart.x;
                float y = io.MousePos.y - EditStart.y;
                if (x * x + y * y >= 16.0f)
                {
                    ImCurveRect<float> selection{{EditStart.x, EditStart.y}, {EditStart.x, EditStart.y}};
                    selection.Expand({io.MousePos.x, io.MousePos.y});
                    for (int point = 0; point < curve.Points.size(); point++)
                    {
                        for (int i = 0; i < 1; i++) // marquee selects keys, not invisible tangent handles
                        {
                            ImCurvePointType type = ImCurvePointType(i);
                    if (GetCurve().Points[point].Points[type].X == kImCurveMax<T>) continue;
                            ImVec2 position = Project(curve.Points[point].Points[type], canvasMin, plotSize);
                            if (selection.Contains({position.x, position.y}))
                            {
                                References.push_back({point, type});
                            }
                        }
                    }
                }
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    Edit = EditType_None;
                }
                break;
            }
            case EditType_MoveCanvas:
            {
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                {
                    Edit = EditType_None;
                    break;
                }
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                Viewport.Min.X -= mouseDelta.X;
                Viewport.Max.X -= mouseDelta.X;
                Viewport.Min.Y -= mouseDelta.Y;
                Viewport.Max.Y -= mouseDelta.Y;
                break;
            }
            }
            if (ImGui::BeginPopup("Point")) {
                if (ImGui::MenuItem("Flat tangents")) {
                    for (const auto& ref : References) {auto& p=curve.Points[ref.Index];p.InSlope=p.OutSlope=T{};p.InterpolationType=ImCurveInterpolationType_Hermite;}
                    IsEditStarted = true;
                }
                if(ImGui::MenuItem("Linear interpolation")) {
                    for(const auto& ref:References)curve.Points[ref.Index].InterpolationType=ImCurveInterpolationType_Linear;
                    IsEditStarted=true;
                }
                if(ImGui::MenuItem("Constant interpolation")) {
                    for(const auto& ref:References)curve.Points[ref.Index].InterpolationType=ImCurveInterpolationType_Square;
                    IsEditStarted=true;
                }
                if (ImGui::MenuItem("Linear tangents")) {
                    for (const auto& ref : References) {
                        auto& p = curve.Points[ref.Index];
                        if (ref.Index > 0) { auto& prev = curve.Points[ref.Index-1]; T dt=p.Points[0].X-prev.Points[0].X; p.InSlope = dt>T{0.000001} ? (p.Points[0].Y-prev.Points[0].Y)/dt : T{}; }
                        if (ref.Index+1 < curve.Points.size()) { auto& next = curve.Points[ref.Index+1]; T dt=next.Points[0].X-p.Points[0].X; p.OutSlope = dt>T{0.000001} ? (next.Points[0].Y-p.Points[0].Y)/dt : T{}; }
                    }
                    IsEditStarted = true;
                }
                ImGui::EndPopup();
            }
            if (isHovered || isActive)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !References.empty())
                {
                    IsEditStarted = true;
                    std::vector<int> points;
                    points.reserve(References.size());
                    for (const Reference& point : References)
                    {
                        points.push_back(point.Index);
                    }
                    std::sort(points.begin(), points.end(), std::greater<int>{});
                    points.erase(std::unique(points.begin(), points.end()), points.end());
                    References.clear();
                    if (points.size() >= curve.Points.size() && !points.empty()) points.pop_back();
                    for (int point : points)
                    {
                        assert(point < curve.Points.size());
                        curve.Points.erase(curve.Points.begin() + point);
                    }
                }
                if (KeyboardHistory && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !io.KeyShift && HistoryIndex > 0)
                {
                    HistoryIndex--;
                    curve = GetCurve();
                    References.clear();
                }
                if (KeyboardHistory && io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))) && HistoryIndex + 1 < History.Size())
                {
                    HistoryIndex++;
                    curve = GetCurve();
                    References.clear();
                }
            }
            if (!curve.Points.empty())
            {
                std::vector<size_t> order(curve.Points.size());
                std::iota(order.begin(), order.end(), 0);
                std::stable_sort(order.begin(), order.end(), [&curve](size_t left, size_t right)
                {
                    return curve.Points[left] < curve.Points[right];
                });
                std::vector<ImCurvePoint<T>> points;
                std::vector<Reference> selectedPoints;
                points.reserve(curve.Points.size());
                selectedPoints.reserve(References.size());
                for (size_t point : order)
                {
                    for (Reference selectedPoint : References)
                    {
                        if (selectedPoint.Index == point)
                        {
                            selectedPoint.Index = int(points.size());
                            selectedPoints.push_back(selectedPoint);
                        }
                    }
                    points.push_back(std::move(curve.Points[point]));
                }
                curve.Points = std::move(points);
                References = std::move(selectedPoints);
                for (size_t point = 0; point < curve.Points.size(); point++)
                {
                    std::optional<ImCurvePoint<T>> end;
                    if (point + 1 < curve.Points.size())
                    {
                        end = curve.Points[point + 1];
                    }
                    curve.Points[point].SetInterpolationType(curve.Points[point].InterpolationType, end);
                }
                if (Normalize) Normalize(curve);
            }
            if (curve != GetCurve())
            {
                if (IsEditStarted)
                {
                    History.Resize(HistoryIndex + 1);
                    History.Add(curve);
                    HistoryIndex = History.Size() - 1;
                    IsEditStarted = false;
                }
                else
                {
                    History[HistoryIndex] = curve;
                }
            }
        }
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(canvasMin, canvasMax, true);
        drawList->AddRectFilled(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
        ImU32 axisColor = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.65f);
        ImU32 sampleColor = Color ? Color : ImGui::GetColorU32(ImGuiCol_PlotLines);
        if (editor)
        {
            ImU32 gridColor = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.15f);
            ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
            T stepX = GetSpacing(Viewport.GetWidth());
            T stepY = GetSpacing(Viewport.GetHeight());
            T stepX1 = std::ceil(Viewport.Min.X / stepX) * stepX;
            T stepY1 = std::ceil(Viewport.Min.Y / stepY) * stepY;
            for (int grid=0; grid<100; ++grid)
            {
                T x = stepX1+T(grid)*stepX;
                if (x > Viewport.Max.X) break;
                float screenX = Project({x, Viewport.Min.Y}, canvasMin, plotSize).x;
                drawList->AddLine({screenX, canvasMin.y}, {screenX, canvasMax.y}, gridColor);
                char buffer[64]; std::snprintf(buffer, sizeof buffer, "%.3g", double(x)); std::string gridLabel(buffer);
                float labelX = std::clamp(screenX + 3.0f, canvasMin.x, canvasMax.x - ImGui::CalcTextSize(gridLabel.data()).x);
                drawList->AddText({labelX, canvasMax.y - ImGui::GetTextLineHeight()}, textColor, gridLabel.data());
            }
            for (int grid=0; grid<100; ++grid)
            {
                T y = stepY1+T(grid)*stepY;
                if (y > Viewport.Max.Y) break;
                float screenY = Project({Viewport.Min.X, y}, canvasMin, plotSize).y;
                drawList->AddLine({canvasMin.x, screenY}, {canvasMax.x, screenY}, gridColor);
                char buffer[64]; std::snprintf(buffer, sizeof buffer, "%.3g", double(y)); std::string gridLabel(buffer);
                float labelY = std::clamp(screenY - ImGui::GetTextLineHeight(), canvasMin.y, canvasMax.y - ImGui::GetTextLineHeight());
                drawList->AddText({canvasMin.x + 3.0f, labelY}, textColor, gridLabel.data());
            }
        }
        if (Viewport.Min.X <= T{} && Viewport.Max.X >= T{})
        {
            float screenX = Project({T{}, Viewport.Min.Y}, canvasMin, plotSize).x;
            drawList->AddLine({screenX, canvasMin.y}, {screenX, canvasMax.y}, axisColor);
        }
        if (Viewport.Min.Y <= T{} && Viewport.Max.Y >= T{})
        {
            float screenY = Project({Viewport.Min.X, T{}}, canvasMin, plotSize).y;
            drawList->AddLine({canvasMin.x, screenY}, {canvasMax.x, screenY}, axisColor);
        }
        static constexpr float kPixelsPerSample = 3.0f;
        int sampleCount = std::max(int(std::ceil(plotSize.x / kPixelsPerSample)), 1);
        std::vector<ImVec2> samples;
        samples.reserve(sampleCount + 2);
        if (GetCurve().Points.empty()) {
            drawList->AddLine(Project({Viewport.Min.X,T{}},canvasMin,plotSize),Project({Viewport.Max.X,T{}},canvasMin,plotSize),sampleColor,2.0f);
        } else {
            const auto& first=GetCurve().Points.front().Points[0];
            const auto& last=GetCurve().Points.back().Points[0];
            if (Viewport.Min.X < first.X) drawList->AddLine(Project({Viewport.Min.X,first.Y},canvasMin,plotSize),Project(first,canvasMin,plotSize),sampleColor,2.0f);
            if (Viewport.Max.X > last.X) drawList->AddLine(Project(last,canvasMin,plotSize),Project({Viewport.Max.X,last.Y},canvasMin,plotSize),sampleColor,2.0f);
        }
        for (size_t startPoint = 0; startPoint + 1 < GetCurve().Points.size(); startPoint++)
        {
            const ImCurvePoint<T>& start = GetCurve().Points[startPoint];
            const ImCurvePoint<T>& end = GetCurve().Points[startPoint + 1];
            T startX = start.Points[ImCurvePointType_Start].X;
            T endX = end.Points[ImCurvePointType_Start].X;
            T width = endX - startX;
            samples.clear();
            samples.push_back(Project(start.Points[ImCurvePointType_Start], canvasMin, plotSize));
            if (width > std::numeric_limits<T>::epsilon())
            {
                for (int sample = 1; sample < sampleCount; sample++)
                {
                    T alpha = T(sample) / T(sampleCount);
                    T x = startX + width * alpha;
                    samples.push_back(Project({x, GetCurve().Sample(x, int(startPoint))}, canvasMin, plotSize));
                }
                if (start.InterpolationType == ImCurveInterpolationType_Square && start.Points[ImCurvePointType_Start].Y != end.Points[ImCurvePointType_Start].Y)
                {
                    samples.push_back(Project({endX, start.Points[ImCurvePointType_Start].Y}, canvasMin, plotSize));
                }
            }
            samples.push_back(Project(end.Points[ImCurvePointType_Start], canvasMin, plotSize));
            drawList->AddPolyline(samples.data(), int(samples.size()), sampleColor, ImDrawFlags_None, 2.0f);
        }
        const float kUnselectedRadius = ImGui::GetFontSize()*0.19f;
        const float kSelectedRadius = ImGui::GetFontSize()*0.35f;
        const float kHighlightedRadius = kSelectedRadius;
        for (T point : highlightedPoints)
        {
            drawList->AddCircleFilled(Project({point, GetCurve().Sample(point)}, canvasMin, plotSize), kHighlightedRadius, sampleColor);
        }
        for (int point = 0; point < GetCurve().Points.size(); point++)
        {
            for (int type = ImCurvePointType_Control; type < ImCurvePointType_Count; ++type) {
                const auto& p = GetCurve().Points[point];
                if (p.Points[type].X == kImCurveMax<T>) continue;
                Reference reference{point, ImCurvePointType(type)};
                const bool selected = std::find(References.begin(), References.end(), Reference{point, ImCurvePointType_Start}) != References.end();
                if (!selected && !(hoveredReference == reference)) continue;
                ImVec2 control = Project(p.Points[type], canvasMin, plotSize);
                drawList->AddLine(Project(p.Points[0], canvasMin, plotSize), control, ImGui::GetColorU32(ImGuiCol_TextDisabled));
                drawList->AddCircleFilled(control, kUnselectedRadius, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
            }
            ImVec2 position = Project(GetCurve().Points[point].Points[ImCurvePointType_Start], canvasMin, plotSize);
            Reference reference{point, ImCurvePointType_Start};
            float radius = kUnselectedRadius;
            if (std::find(References.begin(), References.end(), reference) != References.end() || hoveredReference == reference)
            {
                radius = kSelectedRadius;
            }
            drawList->AddCircleFilled(position, radius, radius == kSelectedRadius ? ImGui::GetColorU32(ImGuiCol_PlotHistogram) : ImGui::GetColorU32(ImGuiCol_Text));
        }
        drawList->AddRect(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_Border), ImGui::GetStyle().FrameRounding);
        drawList->PopClipRect();
        if (editor && Edit == EditType_RectSelect)
        {
            ImCurveRect<float> selection{{EditStart.x, EditStart.y}, {EditStart.x, EditStart.y}};
            selection.Expand({io.MousePos.x, io.MousePos.y});
            drawList->PushClipRect(canvasMin, canvasMax, true);
            drawList->AddRectFilled(selection.Min.To<ImVec2>(), selection.Max.To<ImVec2>(), ImGui::GetColorU32(ImGuiCol_Header, 0.2f));
            drawList->AddRect(selection.Min.To<ImVec2>(), selection.Max.To<ImVec2>(), ImGui::GetColorU32(ImGuiCol_Header));
            drawList->PopClipRect();
        }
        if (Edit == EditType_None)
        {
            IsEditStarted = false;
        }
        ImGui::PopID();
    }

private:
    T GetSpacing(T range)
    {
        T spacing = range / T{10};
        if (spacing <= T{})
        {
            return T{1};
        }
        T magnitude = std::pow(T{10}, std::floor(std::log10(spacing)));
        T normalized = spacing / magnitude;
        if (normalized <= T{1})
        {
            return magnitude;
        }
        else if (normalized <= T{2})
        {
            return T{2} * magnitude;
        }
        else if (normalized <= T{5})
        {
            return T{5} * magnitude;
        }
        else
        {
            return T{10} * magnitude;
        }
    }

    ImVec2 Project(const ImCurveVec2<T>& value, const ImVec2& canvasMin, const ImVec2& plotSize) const
    {
        T x = (value.X - Viewport.Min.X) / Viewport.GetWidth();
        T y = (value.Y - Viewport.Min.Y) / Viewport.GetHeight();
        return {canvasMin.x + kPadding + x * plotSize.x, canvasMin.y + kPadding + (1.0f - y) * plotSize.y};
    }

    ImCurveVec2<T> Unproject(const ImVec2& value, const ImVec2& canvasMin, const ImVec2& plotSize) const
    {
        T x = (value.x - canvasMin.x - kPadding) / plotSize.x;
        T y = 1.0f - (value.y - canvasMin.y - kPadding) / plotSize.y;
        return {Viewport.Min.X + x * Viewport.GetWidth(), Viewport.Min.Y + y * Viewport.GetHeight()};
    }

    ImCurveCircularBuffer<ImCurve<T>, 64> History;
    size_t HistoryIndex;
    std::vector<Reference> References;
    ImCurveRect<T> Viewport;
    ImVec2 EditStart;
    EditType Edit;
    bool IsEditStarted;
    bool IsLoaded;
};
