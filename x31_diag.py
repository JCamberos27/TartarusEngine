import re
root = 'C:/Users/jacob/tbw3c/src/'

def edit(rel, old, new):
    p = root + rel
    with open(p, 'r', encoding='utf-8', newline='') as f:
        s = f.read()
    crlf = '\r\n' in s
    s = s.replace('\r\n', '\n')
    assert s.count(old) == 1, (rel, old[:40])
    s = s.replace(old, new)
    if crlf:
        s = s.replace('\n', '\r\n')
    with open(p, 'w', encoding='utf-8', newline='') as f:
        f.write(s)

edit('Assets/AssetLibrary.h', '    int AsyncInFlight() const;\n',
     '    int AsyncInFlight() const;\n    std::string AsyncDebugDump() const; // X-31 WIP diagnostic\n')

edit('Assets/AssetLibraryAsync.cpp', '''namespace {
AssetLibrary::AsyncHandle ReadyTicket() {''', '''std::string AssetLibrary::AsyncDebugDump() const {
    if (!m_Async) return "(no loader)";
    std::string out;
    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lk(m_Async->Mutex);
        queued = m_Async->Queue.size();
    }
    out += "queue " + std::to_string(queued) + ", active " + std::to_string(m_Async->Active.size()) + "\\n";
    for (const auto& j : m_Async->Active) {
        const char* k = j->K == AsyncJob::Kind::Model ? "model" : j->K == AsyncJob::Kind::Texture ? "tex" : "mat";
        out += std::string("  ") + k + " done=" + (j->Done.load() ? "1" : "0") + " fin=" + (j->Finalized ? "1" : "0") +
               " forget=" + (j->Forget ? "1" : "0") + " gen=" + std::to_string(j->Gen) +
               " model=" + (j->ModelResult ? "1" : "0") + " texOk=" + (j->TexResult.Ok ? "1" : "0") +
               " texPending=" + (j->TexPending ? "1" : "0") + " has=" +
               (j->K == AsyncJob::Kind::Model ? (HasModel(j->Path) ? "1" : "0") : (HasTexture(j->Path) ? "1" : "0")) +
               " " + j->Path + "\\n";
    }
    return out;
}

namespace {
AssetLibrary::AsyncHandle ReadyTicket() {''')

edit('App/Application.cpp', '''                                assets.AsyncInFlight());
                    break;''', '''                                assets.AsyncInFlight());
                    std::printf("[X31] %s", assets.AsyncDebugDump().c_str());
                    std::fflush(stdout);
                    break;''')
print('ok')
