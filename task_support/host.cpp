// Headless host boundary: no CAD geometry, triangulation, Group, or export logic.
#include "solvespace.h"
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>
namespace SolveSpace {
SolveSpaceUI SS = {};
Sketch SK = {};
const std::string &Translate(const char *s) { static std::map<std::string,std::string> strings;return strings.emplace(s,s).first->second; }
const std::string &Translate(const char *,const char *s) { return Translate(s); }
TtfFontList::TtfFontList() : fontLibrary(nullptr), loaded(false) {}
TtfFontList::~TtfFontList() {}
// Exact setting selectors from src/solvespace.cpp:511-521; no UI initialized.
double SolveSpaceUI::ChordTolMm() { return exportMode ? ExportChordTolMm() : chordTolCalculated; }
double SolveSpaceUI::ExportChordTolMm() { return exportChordTol / exportScale; }
int SolveSpaceUI::GetMaxSegments() { return exportMode ? exportMaxSegments : maxSegments; }
// Fixtures supply an already-generated runningShell; no sketch solver is run.
void SolveSpaceUI::GenerateAll(Generate, bool) {}
void SolveSpaceUI::UpdateCenterOfMass() {}
void SolveSpaceUI::ShowNakedEdges(bool) {}
void GraphicsWindow::Invalidate(bool) {}
std::string LoadStringFromGzip(const std::string &) { throw std::runtime_error("embedded web resources unavailable in headless host"); }
std::string LoadString(const std::string &) { throw std::runtime_error("embedded web resources unavailable in headless host"); }
namespace Platform {
class HeadlessDialog final : public MessageDialog {
 std::string message;
 public:
 void SetType(Type) override {} void SetTitle(std::string) override {}
 void SetMessage(std::string s) override { message=std::move(s); }
 void SetDescription(std::string) override {} void AddButton(std::string,Response,bool) override {}
 Response RunModal() override { std::fprintf(stderr,"HOST_DIALOG %s\n",message.c_str());return Response::OK; }
};
MessageDialogRef CreateMessageDialog(WindowRef) { return std::make_shared<HeadlessDialog>(); }
static thread_local std::vector<void*> temporary;
void *AllocTemporary(size_t n) { void *p=std::calloc(1,n);if(!p)throw std::bad_alloc();temporary.push_back(p);return p; }
void FreeAllTemporary() { for(void *p:temporary)std::free(p);temporary.clear(); }
void DebugPrint(const char *fmt, ...) { va_list a;va_start(a,fmt);vfprintf(stderr,fmt,a);va_end(a);fputc('\n',stderr); }
void FatalError(const std::string &s) { throw std::runtime_error(s); }
Path Path::From(std::string s) { Path p;p.raw=std::move(s);return p; }
bool Path::HasExtension(std::string x) const { return std::filesystem::path(raw).extension()=="."+x; }
std::string Path::FileStem() const { return std::filesystem::path(raw).stem().string(); }
std::string Path::FileName() const { return std::filesystem::path(raw).filename().string(); }
Path Path::WithExtension(std::string x) const { auto p=std::filesystem::path(raw);p.replace_extension("."+x);return Path::From(p.string()); }
FILE *OpenFile(const Path &p,const char *mode) { return std::fopen(p.raw.c_str(),mode); }
}
}
