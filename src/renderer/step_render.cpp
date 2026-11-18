// stepthumb-render: reads a STEP file and renders an isometric thumbnail.
//
// Usage:
//   stepthumb-render <input.step | -> <output.tga | -> [size]
//     "-" as input  : STEP data is read from stdin
//     "-" as output : 32-bit TGA (BGRA, top-left origin) is written to stdout
//
// Rendering is pure software (no GPU / OpenGL), so it works inside the
// Windows thumbnail surrogate process and in headless environments.

#include <STEPControl_Reader.hxx>
#include <Interface_Static.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Polygon3D.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <Standard_Failure.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define DUP _dup
#define DUP2 _dup2
#define WRITE _write
#define NULDEV "NUL"
#define OPEN _open
#define CLOSE _close
#else
#include <fcntl.h>
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define WRITE write
#define NULDEV "/dev/null"
#define OPEN open
#define CLOSE close
#endif

namespace {

struct V3 { double x, y, z; };
static inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
static inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static inline V3 norm(V3 a) { double l = std::sqrt(dot(a, a)); return l > 1e-300 ? a * (1.0 / l) : V3{0, 0, 1}; }

struct Vertex { V3 p; V3 n; };
struct Mesh {
  std::vector<Vertex> verts;
  std::vector<int> tris;                       // 3 indices per triangle
  std::vector<std::vector<V3>> edges;          // polylines of B-rep edges
};

// ---------------------------------------------------------------- loading
bool readStep(const std::string& in, const std::string& data, TopoDS_Shape& shape, bool& yUp) {
  STEPControl_Reader reader;
  // A fixed, coarse-ish precision (0.01 mm) makes shape healing during transfer
  // dramatically faster on large files (30 s -> 2.5 s on a 11 MB part); this is
  // far below what a 256 px thumbnail can show.
  Interface_Static::SetIVal("read.precision.mode", 1);
  Interface_Static::SetRVal("read.precision.val", 0.01);
  IFSelect_ReturnStatus st;
  if (in == "-") {
    std::istringstream is(data);
    st = reader.ReadStream("stdin.step", is);
  } else {
    st = reader.ReadFile(in.c_str());
  }
  if (st != IFSelect_RetDone) return false;
  reader.TransferRoots();
  shape = reader.OneShape();
  if (shape.IsNull()) return false;

  // Orientation heuristic: SolidWorks / Creo / Inventor export Y-up, most others Z-up.
  std::string head;
  if (in == "-") head = data.substr(0, 4096);
  else {
    std::ifstream f(in, std::ios::binary);
    head.resize(4096);
    f.read(&head[0], 4096);
    head.resize((size_t)f.gcount());
  }
  std::string low = head;
  std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  yUp = low.find("solidworks") != std::string::npos || low.find("swstep") != std::string::npos ||
        low.find("creo") != std::string::npos || low.find("pro/engineer") != std::string::npos ||
        low.find("inventor") != std::string::npos;
  return true;
}

// ---------------------------------------------------------------- meshing
void buildMesh(const TopoDS_Shape& shape, Mesh& m) {
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  if (box.IsVoid()) return;
  double xmin, ymin, zmin, xmax, ymax, zmax;
  box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
  double diag = std::sqrt((xmax - xmin) * (xmax - xmin) + (ymax - ymin) * (ymax - ymin) + (zmax - zmin) * (zmax - zmin));
  BRepMesh_IncrementalMesh mesher(shape, diag * 0.0015, Standard_False, 0.35, Standard_True);

  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
    const TopoDS_Face& face = TopoDS::Face(ex.Current());
    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    const gp_Trsf tr = loc.Transformation();
    const bool rev = face.Orientation() == TopAbs_REVERSED;
    const int base = (int)m.verts.size();
    const int nn = tri->NbNodes();

    Handle(Geom_Surface) surf;
    try { surf = BRep_Tool::Surface(face); } catch (...) {}
    const bool useUV = tri->HasUVNodes() && !surf.IsNull();

    for (int i = 1; i <= nn; ++i) {
      gp_Pnt p = tri->Node(i).Transformed(tr);
      Vertex v{{p.X(), p.Y(), p.Z()}, {0, 0, 0}};
      if (useUV) {
        gp_Pnt2d uv = tri->UVNode(i);
        GeomLProp_SLProps props(surf, uv.X(), uv.Y(), 1, 1e-9);
        if (props.IsNormalDefined()) {
          gp_Dir d = props.Normal();
          if (rev) d.Reverse();
          d.Transform(tr);
          v.n = {d.X(), d.Y(), d.Z()};
        }
      }
      m.verts.push_back(v);
    }
    // triangles + fallback normals
    std::vector<V3> acc(nn, V3{0, 0, 0});
    for (int t = 1; t <= tri->NbTriangles(); ++t) {
      int a, b, c;
      tri->Triangle(t).Get(a, b, c);
      if (rev) std::swap(b, c);
      a--, b--, c--;
      m.tris.push_back(base + a); m.tris.push_back(base + b); m.tris.push_back(base + c);
      V3 fn = cross(m.verts[base + b].p - m.verts[base + a].p, m.verts[base + c].p - m.verts[base + a].p);
      acc[a] = acc[a] + fn; acc[b] = acc[b] + fn; acc[c] = acc[c] + fn;
    }
    for (int i = 0; i < nn; ++i) {
      Vertex& v = m.verts[base + i];
      V3 fb = norm(acc[i]);
      if (dot(v.n, v.n) < 0.5) v.n = fb;
      else if (dot(v.n, fb) < 0) v.n = v.n * -1.0;   // guard against bad surface orientation
    }
  }

  // B-rep edges (sharp edges + boundaries); skip seams and smooth (tangent) joins.
  TopTools_IndexedDataMapOfShapeListOfShape e2f;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, e2f);
  for (int i = 1; i <= e2f.Extent(); ++i) {
    const TopoDS_Edge& e = TopoDS::Edge(e2f.FindKey(i));
    if (BRep_Tool::Degenerated(e)) continue;
    const TopTools_ListOfShape& faces = e2f.FindFromIndex(i);
    if (faces.Extent() == 0) continue;
    const TopoDS_Face& f1 = TopoDS::Face(faces.First());
    if (faces.Extent() >= 2) {
      const TopoDS_Face& f2 = TopoDS::Face(faces.Last());
      if (f1.IsSame(f2)) continue;  // seam
      GeomAbs_Shape cont = BRep_Tool::Continuity(e, f1, f2);
      if (cont >= GeomAbs_G1) continue;  // tangent join -> no line
    }
    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(f1, loc);
    if (tri.IsNull()) continue;
    Handle(Poly_PolygonOnTriangulation) pot = BRep_Tool::PolygonOnTriangulation(e, tri, loc);
    if (pot.IsNull()) continue;
    const gp_Trsf tr = loc.Transformation();
    std::vector<V3> line;
    const TColStd_Array1OfInteger& idx = pot->Nodes();
    for (int k = idx.Lower(); k <= idx.Upper(); ++k) {
      gp_Pnt p = tri->Node(idx(k)).Transformed(tr);
      line.push_back({p.X(), p.Y(), p.Z()});
    }
    if (line.size() >= 2) m.edges.push_back(std::move(line));
  }
}

// ---------------------------------------------------------------- raster
struct Image {
  int w = 0, h = 0;
  std::vector<float> r, g, b, a, z;  // z: larger = closer
  std::vector<float> nz;              // view-space normal z, for outline detection
  void init(int W, int H) {
    w = W; h = H;
    r.assign((size_t)w * h, 0); g.assign((size_t)w * h, 0); b.assign((size_t)w * h, 0);
    a.assign((size_t)w * h, 0); z.assign((size_t)w * h, -1e30f); nz.assign((size_t)w * h, 0);
  }
};

struct Shading {
  V3 base{0.74, 0.78, 0.84};  // cool light-grey "machined" colour
};

V3 shade(V3 n, const Shading& s) {
  // view space: camera looks along -Z, +Y is up
  if (n.z < 0) n = n * -1.0;                          // two-sided
  const V3 L1 = norm({-0.35, 0.55, 0.75});            // key light, upper left
  const V3 L2 = norm({0.7, 0.05, 0.7});               // fill from right
  const V3 H1 = norm(L1 + V3{0, 0, 1});
  double d1 = std::max(0.0, dot(n, L1));
  double d2 = std::max(0.0, dot(n, L2));
  double hemi = 0.5 + 0.5 * n.y;                      // sky / ground ambient
  double spec = std::pow(std::max(0.0, dot(n, H1)), 40.0) * 0.35;
  double rim = std::pow(1.0 - std::max(0.0, n.z), 3.0) * 0.12;
  double amb = 0.24 + 0.12 * hemi;
  double head = std::max(0.0, n.z);                   // camera headlight (CAD-style)
  double k = amb + 0.42 * head + 0.32 * d1 + 0.14 * d2;
  V3 c = s.base * k + V3{1, 1, 1} * (spec + rim);
  return c;
}

void rasterTriangles(Image& img, const std::vector<V3>& sp, const std::vector<V3>& sn, const std::vector<int>& tris, const Shading& sh) {
  for (size_t t = 0; t + 2 < tris.size(); t += 3) {
    const V3 &A = sp[tris[t]], &B = sp[tris[t + 1]], &C = sp[tris[t + 2]];
    double area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
    if (std::fabs(area) < 1e-12) continue;
    int x0 = std::max(0, (int)std::floor(std::min({A.x, B.x, C.x})));
    int x1 = std::min(img.w - 1, (int)std::ceil(std::max({A.x, B.x, C.x})));
    int y0 = std::max(0, (int)std::floor(std::min({A.y, B.y, C.y})));
    int y1 = std::min(img.h - 1, (int)std::ceil(std::max({A.y, B.y, C.y})));
    if (x0 > x1 || y0 > y1) continue;
    const V3 &nA = sn[tris[t]], &nB = sn[tris[t + 1]], &nC = sn[tris[t + 2]];
    double inv = 1.0 / area;
    for (int y = y0; y <= y1; ++y) {
      double py = y + 0.5;
      for (int x = x0; x <= x1; ++x) {
        double px = x + 0.5;
        double w0 = ((B.x - px) * (C.y - py) - (B.y - py) * (C.x - px)) * inv;
        double w1 = ((C.x - px) * (A.y - py) - (C.y - py) * (A.x - px)) * inv;
        double w2 = 1.0 - w0 - w1;
        if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
        float zz = (float)(w0 * A.z + w1 * B.z + w2 * C.z);
        size_t i = (size_t)y * img.w + x;
        if (zz <= img.z[i]) continue;
        img.z[i] = zz;
        V3 n = norm(nA * w0 + nB * w1 + nC * w2);
        V3 c = shade(n, sh);
        img.r[i] = (float)c.x; img.g[i] = (float)c.y; img.b[i] = (float)c.z; img.a[i] = 1.f;
        img.nz[i] = (float)std::fabs(n.z);
      }
    }
  }
}

void drawLine(Image& img, V3 a, V3 b, float bias, float thick, V3 col, float alpha) {
  double dx = b.x - a.x, dy = b.y - a.y;
  int steps = (int)std::ceil(std::max(std::fabs(dx), std::fabs(dy)) * 2) + 1;
  int rad = (int)std::ceil(thick);
  for (int s = 0; s <= steps; ++s) {
    double t = (double)s / steps;
    double x = a.x + dx * t, y = a.y + dy * t, z = a.z + (b.z - a.z) * t;
    for (int oy = -rad; oy <= rad; ++oy)
      for (int ox = -rad; ox <= rad; ++ox) {
        int px = (int)std::floor(x) + ox, py = (int)std::floor(y) + oy;
        if (px < 0 || py < 0 || px >= img.w || py >= img.h) continue;
        double ddx = px + 0.5 - x, ddy = py + 0.5 - y;
        if (ddx * ddx + ddy * ddy > thick * thick) continue;
        size_t i = (size_t)py * img.w + px;
        if (img.a[i] <= 0) continue;                 // only on the part itself
        if (z + bias < img.z[i]) continue;           // hidden
        img.r[i] = img.r[i] * (1 - alpha) + (float)col.x * alpha;
        img.g[i] = img.g[i] * (1 - alpha) + (float)col.y * alpha;
        img.b[i] = img.b[i] * (1 - alpha) + (float)col.z * alpha;
      }
  }
}

bool render(const Mesh& m, bool yUp, int size, std::vector<unsigned char>& bgra) {
  if (m.tris.empty()) return false;
  // Isometric camera. World up axis: Y or Z.
  V3 up = yUp ? V3{0, 1, 0} : V3{0, 0, 1};
  V3 eye = yUp ? norm({1, 0.82, 1}) : norm({1, -1, 0.82});   // direction from target to eye
  // SolidWorks iso looks from +X,+Y,+Z (Y up). For Z-up use front-right-top.
  V3 fwd = eye * -1.0;
  V3 right = norm(cross(fwd, up));
  V3 vup = cross(right, fwd);

  // Supersampling for anti-aliasing; lower for big sizes to bound memory (~30 MB max).
  const int SS = size <= 256 ? 3 : (size <= 512 ? 2 : 1);
  const int W = size * SS;
  std::vector<V3> sp(m.verts.size()), sn(m.verts.size());
  double minx = 1e300, maxx = -1e300, miny = 1e300, maxy = -1e300;
  for (size_t i = 0; i < m.verts.size(); ++i) {
    const V3& p = m.verts[i].p;
    V3 v{dot(p, right), dot(p, vup), dot(p, eye)};
    sp[i] = v;
    sn[i] = V3{dot(m.verts[i].n, right), dot(m.verts[i].n, vup), dot(m.verts[i].n, eye)};
    minx = std::min(minx, v.x); maxx = std::max(maxx, v.x);
    miny = std::min(miny, v.y); maxy = std::max(maxy, v.y);
  }
  double ext = std::max(maxx - minx, maxy - miny);
  if (ext <= 0) return false;
  const double margin = 0.06;
  double scale = W * (1 - 2 * margin) / ext;
  double cx = (minx + maxx) * 0.5, cy = (miny + maxy) * 0.5;
  auto toScreen = [&](V3 v) { return V3{(v.x - cx) * scale + W * 0.5, W * 0.5 - (v.y - cy) * scale, v.z * scale}; };
  for (auto& v : sp) v = toScreen(v);

  Image img;
  img.init(W, W);
  Shading sh;
  rasterTriangles(img, sp, sn, m.tris, sh);

  // B-rep edge lines
  const V3 edgeCol{0.16, 0.18, 0.22};
  const float bias = (float)(0.004 * W);
  for (const auto& line : m.edges) {
    for (size_t k = 1; k < line.size(); ++k) {
      V3 a = line[k - 1], b = line[k];
      V3 A{dot(a, right), dot(a, vup), dot(a, eye)}, B{dot(b, right), dot(b, vup), dot(b, eye)};
      drawLine(img, toScreen(A), toScreen(B), bias, 0.85f * SS * 0.5f + 0.3f, edgeCol, 0.75f);
    }
  }

  // Silhouette / depth-discontinuity outline
  std::vector<float> outline((size_t)W * W, 0.f);
  const float zjump = (float)(0.02 * W);
  for (int y = 0; y < W; ++y)
    for (int x = 0; x < W; ++x) {
      size_t i = (size_t)y * W + x;
      if (img.a[i] <= 0) continue;
      bool edge = false;
      for (int k = 0; k < 4 && !edge; ++k) {
        int nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3);
        if (nx < 0 || ny < 0 || nx >= W || ny >= W) { edge = true; break; }
        size_t j = (size_t)ny * W + nx;
        if (img.a[j] <= 0 || img.z[j] - img.z[i] > zjump) edge = true;
      }
      if (edge) outline[i] = 1.f;
    }
  for (size_t i = 0; i < outline.size(); ++i)
    if (outline[i] > 0) {
      img.r[i] = img.r[i] * 0.2f + (float)edgeCol.x * 0.8f;
      img.g[i] = img.g[i] * 0.2f + (float)edgeCol.y * 0.8f;
      img.b[i] = img.b[i] * 0.2f + (float)edgeCol.z * 0.8f;
    }

  // Downsample (box filter) -> premultiplied BGRA
  bgra.assign((size_t)size * size * 4, 0);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      double r = 0, g = 0, b = 0, a = 0;
      for (int sy = 0; sy < SS; ++sy)
        for (int sx = 0; sx < SS; ++sx) {
          size_t i = (size_t)(y * SS + sy) * W + (x * SS + sx);
          double al = img.a[i];
          r += std::min(1.f, img.r[i]) * al; g += std::min(1.f, img.g[i]) * al; b += std::min(1.f, img.b[i]) * al; a += al;
        }
      const double n = SS * SS;
      unsigned char* o = &bgra[((size_t)y * size + x) * 4];
      auto to8 = [](double v) { return (unsigned char)std::lround(std::max(0.0, std::min(1.0, v)) * 255.0); };
      o[0] = to8(b / n); o[1] = to8(g / n); o[2] = to8(r / n); o[3] = to8(a / n);
    }
  return true;
}

std::string makeTga(int size, const std::vector<unsigned char>& bgra) {
  unsigned char h[18] = {0};
  h[2] = 2;                       // uncompressed true-colour
  h[12] = size & 0xFF; h[13] = (size >> 8) & 0xFF;
  h[14] = size & 0xFF; h[15] = (size >> 8) & 0xFF;
  h[16] = 32;                     // bpp
  h[17] = 0x28;                   // 8 alpha bits, top-left origin
  // TGA stores straight (non-premultiplied) alpha; un-premultiply.
  std::vector<unsigned char> out(bgra);
  for (size_t i = 0; i < out.size(); i += 4) {
    unsigned a = out[i + 3];
    if (a && a < 255)
      for (int c = 0; c < 3; ++c) out[i + c] = (unsigned char)std::min(255u, (out[i + c] * 255u + a / 2) / a);
  }
  std::string res((const char*)h, 18);
  res.append((const char*)out.data(), out.size());
  return res;
}

// Keep the real stdout for our TGA and send everything else (stray library
// prints to stdout) to the null device, so the image stream can't be corrupted.
int detachStdout() {
  std::fflush(stdout);
  int fd = DUP(1);
  int nul = OPEN(NULDEV, O_WRONLY);
  if (nul >= 0) { DUP2(nul, 1); CLOSE(nul); }
  return fd;
}

bool writeAll(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    int chunk = (int)std::min<size_t>(s.size() - off, 1 << 20);
    int w = (int)WRITE(fd, s.data() + off, chunk);
    if (w <= 0) return false;
    off += (size_t)w;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <input.step|-> <output.tga|-> [size]\n", argv[0]);
    return 2;
  }
  std::string in = argv[1], out = argv[2];
  int size = argc > 3 ? std::atoi(argv[3]) : 256;
  size = std::max(16, std::min(1024, size));

#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  int outFd = -1;
  if (out == "-") {
    outFd = detachStdout();
#ifdef _WIN32
    _setmode(outFd, _O_BINARY);
#endif
  }
  // Silence OCCT's console chatter (it would corrupt stdout output).
  {
    Handle(Message_Messenger) msg = Message::DefaultMessenger();
    msg->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
  }

  try {
    std::string data;
    if (in == "-") data.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    auto T0 = std::chrono::steady_clock::now();
    const bool prof = std::getenv("STEPTHUMB_PROFILE") != nullptr;
    auto lap = [&](const char* what) {
      if (!prof) return;
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
      std::fprintf(stderr, "[%7.2fs] %s\n", s, what);
    };
    TopoDS_Shape shape;
    bool yUp = false;
    if (!readStep(in, data, shape, yUp)) { std::fprintf(stderr, "cannot read STEP\n"); return 3; }
    data.clear(); data.shrink_to_fit();
    lap("read+transfer");
    Mesh mesh;
    buildMesh(shape, mesh);
    lap("mesh");
    std::vector<unsigned char> bgra;
    if (!render(mesh, yUp, size, bgra)) { std::fprintf(stderr, "nothing to render\n"); return 4; }
    lap("render");
    const std::string tga = makeTga(size, bgra);
    if (outFd >= 0) { if (!writeAll(outFd, tga)) return 6; }
    else {
      std::ofstream f(out, std::ios::binary);
      f.write(tga.data(), (std::streamsize)tga.size());
      if (!f) return 6;
    }
  } catch (const Standard_Failure& e) {
    std::fprintf(stderr, "OCCT error: %s\n", e.GetMessageString());
    return 5;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 5;
  } catch (...) {
    std::fprintf(stderr, "unknown error\n");
    return 5;
  }
  return 0;
}
