// Prints a hash of the generated GLES text of every ShaderUid in a GLES
// program cache (shaders.bin), plus of the same uid as a VR cutaway variant
// and of the fixed copy shaders when the generator has them.  Built against
// two versions of gl_shadergen.cpp, the outputs must be identical: that is the
// check that the GLES dialect stays byte-identical.
//   gles_hashes <shaders.bin> [dumpdir]
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "gl_shadergen.h"

static uint64_t fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (char c : s) h = (h ^ (uint8_t)c) * 1099511628211ull;
    return h;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: gles_hashes <shaders.bin> [dumpdir]\n");
        return 2;
    }
    FILE* in = fopen(argv[1], "rb");
    uint32_t magic = 0;
    if (!in || fread(&magic, 4, 1, in) != 1 || magic != 0x32434750) {
        fprintf(stderr, "not a GLES program cache: %s\n", argv[1]);
        return 1;
    }
    int n = 0;
    for (;;) {
        uint32_t hdr[3];
        uint64_t srcHash;
        gpu::ShaderUid uid;
        if (fread(hdr, 4, 3, in) != 3 || hdr[0] != sizeof(uid) || hdr[2] > (64u << 20)) break;
        if (fread(&srcHash, 8, 1, in) != 1 || fread(&uid, sizeof(uid), 1, in) != 1 || fseek(in, (long)hdr[2], SEEK_CUR) != 0) break;
        for (int cut = 0; cut < 2; cut++) {
            gpu::ShaderUid u = uid;
            if (cut) u.cutaway = 1;
            std::string vs = gpu::genVertexShader(u), fs = gpu::genFragmentShader(u);
            printf("%d %d %016llx %016llx\n", n, cut, (unsigned long long)fnv(vs), (unsigned long long)fnv(fs));
            if (argc > 2 && !cut) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%d.vert", argv[2], n);
                FILE* f = fopen(path, "wb");
                if (f) { fwrite(vs.data(), 1, vs.size(), f); fclose(f); }
                snprintf(path, sizeof(path), "%s/%d.frag", argv[2], n);
                f = fopen(path, "wb");
                if (f) { fwrite(fs.data(), 1, fs.size(), f); fclose(f); }
            }
        }
        n++;
    }
    fclose(in);
    fprintf(stderr, "%d programs\n", n);
    return 0;
}
