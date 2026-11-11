// STEP loader, built on OpenCASCADE: reads the B-rep with its assembly structure
// and colors, tessellates it, and emits smooth-shaded triangles.
#include "mesh.hpp"

#include <BRepBndLib.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <NCollection_DataMap.hxx>
#include <Poly_Triangulation.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

namespace ql3d {
namespace {

using FaceColors = NCollection_DataMap<TopoDS_Shape, uint32_t, TopTools_ShapeMapHasher>;

struct Ctx {
    Handle(XCAFDoc_ColorTool) colors;
    Mesh &out;
};

bool labelColor(const Ctx &c, const TDF_Label &label, uint32_t &color) {
    Quantity_Color q;
    if (!c.colors->GetColor(label, XCAFDoc_ColorSurf, q) && !c.colors->GetColor(label, XCAFDoc_ColorGen, q)) return false;
    Standard_Real r, g, b;
    q.Values(r, g, b, Quantity_TOC_sRGB);
    color = rgb((uint32_t)(r * 255 + 0.5), (uint32_t)(g * 255 + 0.5), (uint32_t)(b * 255 + 0.5));
    return true;
}

V3 v3(const gp_XYZ &p) { return {(float)p.X(), (float)p.Y(), (float)p.Z()}; }

// Emits the triangulation of one part. Colors set on individual faces or solids override the part's color.
void emitShape(Ctx &c, const TDF_Label &label, const TopLoc_Location &loc, uint32_t color) {
    TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(label);
    if (shape.IsNull()) return;

    FaceColors faceColors;
    TDF_LabelSequence subs;
    XCAFDoc_ShapeTool::GetSubShapes(label, subs);
    for (int pass = 0; pass < 2; pass++)  // solids/shells first, then single faces on top
        for (Standard_Integer i = 1; i <= subs.Length(); i++) {
            uint32_t sc;
            if (!labelColor(c, subs.Value(i), sc)) continue;
            TopoDS_Shape sub = XCAFDoc_ShapeTool::GetShape(subs.Value(i));
            if (sub.IsNull() || (sub.ShapeType() == TopAbs_FACE) != (pass == 1)) continue;
            for (TopExp_Explorer ex(sub, TopAbs_FACE); ex.More(); ex.Next()) faceColors.Bind(ex.Current(), sc);
        }

    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face &face = TopoDS::Face(ex.Current());
        TopLoc_Location faceLoc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, faceLoc);
        if (tri.IsNull()) continue;
        BRepLib_ToolTriangulatedShape::ComputeNormals(face, tri);
        bool hasNormals = tri->HasNormals();

        gp_Trsf trsf = (loc * faceLoc).Transformation();
        bool reversed = face.Orientation() == TopAbs_REVERSED;
        bool flip = reversed != trsf.IsNegative();
        uint32_t fc = color;
        if (const uint32_t *found = faceColors.Seek(face)) fc = *found;
        uint32_t cs[3] = {fc, fc, fc};

        for (Standard_Integer t = 1; t <= tri->NbTriangles(); t++) {
            Standard_Integer idx[3];
            tri->Triangle(t).Get(idx[0], idx[1], idx[2]);
            if (flip) std::swap(idx[1], idx[2]);
            V3 p[3], n[3];
            for (int k = 0; k < 3; k++) {
                p[k] = v3(tri->Node(idx[k]).Transformed(trsf).XYZ());
                if (hasNormals) {
                    gp_Dir d = tri->Normal(idx[k]);
                    if (reversed) d.Reverse();
                    n[k] = v3(d.Transformed(trsf).XYZ());
                }
            }
            if (!hasNormals) n[0] = n[1] = n[2] = faceNormal(p[0], p[1], p[2]);
            c.out.tri(p[0], p[1], p[2], n[0], n[1], n[2], cs);
        }
    }
}

// Walks the assembly tree, accumulating placement and inheriting colors downward.
void walk(Ctx &c, const TDF_Label &label, const TopLoc_Location &loc, uint32_t inherited, int depth) {
    if (depth > 64) return;
    TDF_Label ref = label;
    TopLoc_Location here = loc;
    uint32_t color = inherited, own;
    bool hasOwn = labelColor(c, label, own);
    if (XCAFDoc_ShapeTool::IsReference(label)) {
        here = loc * XCAFDoc_ShapeTool::GetLocation(label);
        XCAFDoc_ShapeTool::GetReferredShape(label, ref);
        if (!hasOwn) hasOwn = labelColor(c, ref, own);
    }
    if (hasOwn) color = own;
    if (XCAFDoc_ShapeTool::IsAssembly(ref)) {
        TDF_LabelSequence comps;
        XCAFDoc_ShapeTool::GetComponents(ref, comps);
        for (Standard_Integer i = 1; i <= comps.Length(); i++) walk(c, comps.Value(i), here, color, depth + 1);
    } else {
        emitShape(c, ref, here, color);
    }
}

}  // namespace

bool loadSTEP(const char *path, Mesh &out, std::string &err) {
    try {
        Handle(TDocStd_Document) doc = new TDocStd_Document("BinXCAF");
        STEPCAFControl_Reader reader;
        reader.SetColorMode(true);
        reader.SetNameMode(false);
        reader.SetLayerMode(false);
        if (reader.ReadFile(path) != IFSelect_RetDone) { err = "Could not parse STEP file"; return false; }

        // Shape healing dominates import time. Keep the wire/edge fixes the mesher depends on
        // (e.g. building missing pcurves) and skip the ones that only matter for modeling.
        // Must come after ReadFile: the parameters live on the transfer actor created there.
        DE_ShapeFixParameters fix;
        const auto off = DE_ShapeFixParameters::FixMode::NotFix;
        fix.FixShellOrientationMode = off;
        fix.FixFaceOrientationMode = off;
        fix.FixSameParameterMode = off;
        fix.FixSelfIntersectionMode = off;
        fix.FixIntersectingWiresMode = off;
        fix.FixSelfIntersectingEdgeMode = off;
        fix.FixIntersectingEdgesMode = off;
        fix.FixNonAdjacentIntersectingEdgesMode = off;
        fix.FixNotchedEdgesMode = off;
        fix.FixSmallAreaWireMode = off;
        fix.RemoveSmallAreaFaceMode = off;
        reader.SetShapeFixParameters(fix);

        if (!reader.Transfer(doc)) { err = "Could not convert STEP geometry"; return false; }

        Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        TDF_LabelSequence roots;
        shapes->GetFreeShapes(roots);

        // Tessellation tolerance relative to overall model size.
        Bnd_Box box;
        for (Standard_Integer i = 1; i <= roots.Length(); i++)
            BRepBndLib::Add(XCAFDoc_ShapeTool::GetShape(roots.Value(i)), box, false);
        if (box.IsVoid()) { err = "The file contains no geometry"; return false; }
        double deflection = std::sqrt(box.SquareExtent()) * 0.001;
        if (deflection < 1e-5) deflection = 1e-5;
        for (Standard_Integer i = 1; i <= roots.Length(); i++)
            BRepMesh_IncrementalMesh(XCAFDoc_ShapeTool::GetShape(roots.Value(i)), deflection, false, 0.35, true);

        Ctx c{XCAFDoc_DocumentTool::ColorTool(doc->Main()), out};
        for (Standard_Integer i = 1; i <= roots.Length(); i++) walk(c, roots.Value(i), TopLoc_Location(), kNoColor, 0);
        return true;
    } catch (const Standard_Failure &f) {
        err = std::string("STEP error: ") + (f.GetMessageString() ? f.GetMessageString() : "unknown");
    } catch (...) {
        err = "STEP error";
    }
    return false;
}

}  // namespace ql3d
