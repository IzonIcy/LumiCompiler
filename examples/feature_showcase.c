#define GLUE(a, b) a ## b

typedef unsigned long ulong;

static inline ulong mix(ulong seed, double scale) {
    const char *message = u8"lexer demo\n";
    const char suffix = '\x41';
    ulong flags = 0b101101u + 0xFFull + 0777u;
    double ratio = .125e+2 + 0x1.fp3 + scale;

    for (int i = 0; i < 4; ++i) {
        flags <<= 1;
        if ((flags & 0x10u) != 0u && message[i] != '\0') {
            seed ^= (ulong)message[i];
        }
    }

    return seed ? seed : (ulong)(ratio + suffix);
}

int main(void) {
    return (int)mix(7u, 0.25);
}
