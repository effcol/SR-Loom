#include "../src/LFPReader.h"
#include <cstdio>
int main(int argc, char** argv) {
    srw::LFPReader r;
    if (!r.LoadFromFile(argv[1])) return 1;
    auto top = r.TopLevelJson();
    auto ref = srw::LFPJsonFindString(top, argv[2]);
    auto* c = r.FindBySha1(ref);
    if (!c) { fprintf(stderr, "missing\n"); return 2; }
    fwrite(c->data.data(), 1, c->data.size(), stdout);
    return 0;
}