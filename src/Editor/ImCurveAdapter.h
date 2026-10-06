#pragma once
#include "Curve.h"
#include "../../extern/imcurve/imcurve.hpp"
namespace CurveEditor {
inline void Normalize(ImCurve<float>& result) {
    for (size_t i=0; i<result.Points.size(); ++i) {
        auto& p=result.Points[i];
        p.Points[1]=p.Points[2]=kImCurveVec2Max<float>;
        if (p.InterpolationType==ImCurveInterpolationType_Hermite && i+1<result.Points.size()) {
            float dx=(result.Points[i+1].Points[0].X-p.Points[0].X)/3;
            p.Points[1]={p.Points[0].X+dx,p.Points[0].Y+dx*p.OutSlope};
        }
        if (i>0 && result.Points[i-1].InterpolationType==ImCurveInterpolationType_Hermite) {
            float dx=(result.Points[i-1].Points[0].X-p.Points[0].X)/3;
            p.Points[2]={p.Points[0].X+dx,p.Points[0].Y+dx*p.InSlope};
        }
    }
}
inline ImCurve<float> ToImCurve(const Curve& curve) {
    ImCurve<float> result;
    for (const auto& k:curve.Keys) {
        ImCurvePoint<float> p; p.Points[0]={k.Time,k.Value};
        p.InSlope=k.InTangent; p.OutSlope=k.OutTangent; result.Points.push_back(p);
        result.Points.back().InterpolationType=k.Interpolation==CurveInterpolation::Linear?ImCurveInterpolationType_Linear:
            k.Interpolation==CurveInterpolation::Constant?ImCurveInterpolationType_Square:ImCurveInterpolationType_Hermite;
    }
    Normalize(result); return result;
}
inline Curve FromImCurve(const ImCurve<float>& curve) {
    Curve result;
    for (const auto& p:curve.Points) result.Keys.push_back({p.Points[0].X,p.Points[0].Y,p.InSlope,p.OutSlope,
        p.InterpolationType==ImCurveInterpolationType_Linear?CurveInterpolation::Linear:
        p.InterpolationType==ImCurveInterpolationType_Square?CurveInterpolation::Constant:CurveInterpolation::Cubic});
    return result;
}
inline void Insert(ImCurve<float>& curve,float time) {
    for (const auto& p:curve.Points) if (std::abs(p.Points[0].X-time)<0.0001f) return;
    Curve engine=FromImCurve(curve);
    // Runtime holds the boundary value outside the keys. Extend that hold with flat slopes.
    if (!engine.Keys.empty() && time<engine.Keys.front().Time) engine.Keys.front().InTangent=0;
    if (!engine.Keys.empty() && time>engine.Keys.back().Time) engine.Keys.back().OutTangent=0;
    float slope=0;
    CurveInterpolation mode=CurveInterpolation::Cubic;
    for (size_t i=1;i<engine.Keys.size();++i) {
        const auto& a=engine.Keys[i-1]; const auto& b=engine.Keys[i];
        if (time>a.Time && time<b.Time) {
            mode=a.Interpolation;
            float dt=b.Time-a.Time,t=(time-a.Time)/dt,t2=t*t;
            slope=mode==CurveInterpolation::Constant?0:mode==CurveInterpolation::Linear?(b.Value-a.Value)/dt:
                ((6*t2-6*t)*a.Value+(-6*t2+6*t)*b.Value)/dt
                +(3*t2-4*t+1)*a.OutTangent+(3*t2-2*t)*b.InTangent;
            break;
        }
    }
    int index=engine.AddKey(time);
    engine.Keys[index].InTangent=engine.Keys[index].OutTangent=slope;
    engine.Keys[index].Interpolation=mode;
    curve=ToImCurve(engine);
}
}
