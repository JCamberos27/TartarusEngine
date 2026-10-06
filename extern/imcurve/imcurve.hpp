#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

enum ImCurveInterpolationType
{
    ImCurveInterpolationType_Square,
    ImCurveInterpolationType_Linear,
    ImCurveInterpolationType_Quadratic,
    ImCurveInterpolationType_Hermite,
};

static constexpr const char* kImCurveInterpolationType[] =
{
    "Square",
    "Linear",
    "Quadratic",
    "Cubic Hermite",
};

enum ImCurvePointType
{
    ImCurvePointType_Start,
    ImCurvePointType_Control,
    ImCurvePointType_InControl,
    ImCurvePointType_Count,
};

template<typename T>
struct ImCurveVec2
{
    template<typename U>
    U To() const
    {
        return U{X, Y};
    }

    bool operator==(const ImCurveVec2<T>& other) const
    {
        return X == other.X && Y == other.Y;
    }

    T X;
    T Y;
};

template<typename T>
inline constexpr T kImCurveMax{std::numeric_limits<T>::max()};

template<typename T>
inline constexpr ImCurveVec2<T> kImCurveVec2Max{kImCurveMax<T>, kImCurveMax<T>};

template<typename T>
struct ImCurveRect
{
    T GetWidth() const
    {
        return Max.X - Min.X;
    }

    T GetHeight() const
    {
        return Max.Y - Min.Y;
    }

    bool Contains(const ImCurveVec2<T>& point) const
    {
        return point.X >= Min.X && point.X <= Max.X && point.Y >= Min.Y && point.Y <= Max.Y;
    }

    void Expand(const ImCurveVec2<T>& point)
    {
        Min.X = std::min(Min.X, point.X);
        Min.Y = std::min(Min.Y, point.Y);
        Max.X = std::max(Max.X, point.X);
        Max.Y = std::max(Max.Y, point.Y);
    }

    ImCurveVec2<T> Min;
    ImCurveVec2<T> Max;
};

template<typename T>
struct ImCurvePoint
{
    ImCurvePoint()
        : Points{kImCurveVec2Max<T>, kImCurveVec2Max<T>, kImCurveVec2Max<T>}
        , InterpolationType{ImCurveInterpolationType_Linear}
    {
    }

    T Interpolate(const ImCurvePoint<T>& other, T x) const
    {
        const ImCurveVec2<T>& start = Points[ImCurvePointType_Start];
        const ImCurveVec2<T>& control = Points[ImCurvePointType_Control];
        const ImCurveVec2<T>& end = other.Points[ImCurvePointType_Start];
        T width = end.X - start.X;
        if (width <= T{0.000001})
        {
            return end.Y;
        }
        T alpha = (x - start.X) / width;
        switch (InterpolationType)
        {
        case ImCurveInterpolationType_Hermite:
        {
            T t2 = alpha * alpha, t3 = t2 * alpha;
            return (2*t3-3*t2+1)*start.Y + (t3-2*t2+alpha)*width*OutSlope
                + (-2*t3+3*t2)*end.Y + (t3-t2)*width*other.InSlope;
        }
        case ImCurveInterpolationType_Square:
        {
            return x == end.X ? end.Y : start.Y;
        }
        case ImCurveInterpolationType_Linear:
        {
            return start.Y + (end.Y - start.Y) * alpha;
        }
        case ImCurveInterpolationType_Quadratic:
        {
            if (x == start.X)
            {
                return start.Y;
            }
            if (x == end.X)
            {
                return end.Y;
            }
            T x0 = start.X;
            T x1 = control.X;
            T x2 = end.X;
            T y0 = start.Y;
            T y1 = control.Y;
            T y2 = end.Y;
            // https://en.wikipedia.org/wiki/B%C3%A9zier_curve
            // x(t) = ((1-t)^2)x0 + 2(1-t)tx1 + (t^2)x2
            // x(t) = (1-2t+t^2)x0 + (2 - 2t)tx1 + (t^2)x2
            // x(t) = x0 - 2tx0 + (t^2)x0 + 2tx1 - 2(t^2)x1 + (t^2)x2
            // x(t) = (t^2)(x0-2x1+x2) + t(-2x0 + 2x1) + x0
            // x(t) = (t^2)(x0-2x1+x2) + 2t(x1 - x0) + x0
            // x(t) = x
            // 0 = (t^2)(x0-2x1+x2) + 2t(x1 - x0) + x0-x
            // 0 = a(t^2) + bt + c
            // a = (x0 - 2x1 + x2)
            // b = 2(x1 - x0)
            // c = x0 - x
            T a = x0 - (T{2} * x1) + x2;
            T b = T{2} * (x1 - x0);
            T c = x0 - x;
            T epsilon = std::numeric_limits<T>::epsilon() * std::max({T{1}, std::abs(x0), std::abs(x1), std::abs(x2)});
            T t;
            if (std::abs(a) <= epsilon)
            {
                t = -c / b;
            }
            else
            {
                // https://en.wikipedia.org/wiki/Quadratic_formula
                T discriminant = b * b - T{4} * a * c;
                assert(discriminant >= -epsilon);
                T sqrt = std::sqrt(std::max(discriminant, T{}));
                T denominator = T{2} * a;
                t = (-b - sqrt) / denominator;
                if (t < T{} || t > T{1})
                {
                    t = (-b + sqrt) / denominator;
                    t = std::clamp(t, T{}, T{1});
                }
            }
            // y(t) = ((1-t)^2)y0 + 2(1-t)ty1 + (t^2)y2
            T invT = T{1} - t;
            return invT*invT*y0 + T{2}*invT*t*y1 + t*t*y2;
        }
        }
        assert(false);
        return T{};
    }

    void SetInterpolationType(ImCurveInterpolationType interpolationType, std::optional<ImCurvePoint<T>> end = {})
    {
        InterpolationType = interpolationType;
        if (interpolationType == ImCurveInterpolationType_Hermite) return;
        if (interpolationType == ImCurveInterpolationType_Quadratic && end)
        {
            const ImCurveVec2<T>& start = Points[ImCurvePointType_Start];
            const ImCurveVec2<T>& finish = end->Points[ImCurvePointType_Start];
            ImCurveVec2<T>& control = Points[ImCurvePointType_Control];
            if (!HasControl())
            {
                control = {(start.X + finish.X) / T{2}, (start.Y + finish.Y) / T{2}};
            }
            control.X = std::clamp(control.X, start.X, finish.X);
        }
        else
        {
            Points[ImCurvePointType_Control] = kImCurveVec2Max<T>;
        }
    }

    bool HasControl() const
    {
        return Points[ImCurvePointType_Control].X != kImCurveMax<T>;
    }

    bool operator<(const ImCurvePoint<T>& other) const
    {
        return Points[ImCurvePointType_Start].X < other.Points[ImCurvePointType_Start].X;
    }
    bool operator==(const ImCurvePoint<T>& other) const
    {
        return Points == other.Points && InterpolationType == other.InterpolationType && InSlope == other.InSlope && OutSlope == other.OutSlope;
    }

    std::array<ImCurveVec2<T>, ImCurvePointType_Count> Points;
    ImCurveInterpolationType InterpolationType;
    T InSlope{}, OutSlope{};
};

template<typename T>
struct ImCurve
{
    T Sample(T x, int index = -1) const
    {
        if (Points.empty())
        {
            return T{};
        }
        if (!(x > Points.front().Points[0].X)) return Points.front().Points[0].Y;
        if (x >= Points.back().Points[0].X) return Points.back().Points[0].Y;
        size_t point;
        if (index >= 0)
        {
            point = index;
            assert(point + 1 < Points.size());
        }
        else
        {
            auto right = std::lower_bound(Points.begin(), Points.end(), x, [](const ImCurvePoint<T>& point, T value)
            {
                return point.Points[ImCurvePointType_Start].X < value;
            });
            if (right == Points.begin())
            {
                return Points.front().Points[ImCurvePointType_Start].Y;
            }
            else if (right == Points.end())
            {
                return Points.back().Points[ImCurvePointType_Start].Y;
            }
            else
            {
                point = right - Points.begin() - 1;
            }
        }
        return Points[point].Interpolate(Points[point + 1], x);
    }

    bool operator==(const ImCurve<T>& other) const
    {
        return Points == other.Points;
    }

    bool operator!=(const ImCurve<T>& other) const { return !(*this == other); }
    std::vector<ImCurvePoint<T>> Points;
};
