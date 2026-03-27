int main(void) {
    const char *broken = "unterminated
    int bad_hex = 0x;
    int bad_bin = 0b102;
    double bad_exp = 12e+;
    char weird = '\q';
}

/* forgot to close this block comment
