//#Config:clang
//#Compiler: clang
//#CompArgs:-ggdb
//#LinkerDriver:clang
//#LinkArgs:-z now -Wl,--gdb-index
//#SkipLinker:ld
//#EnableLinker:lld
//#DiffIgnore:section.relro_padding
//#DiffIgnore:section.got.plt.entsize
//#DiffIgnore:section.rodata
//#DiffIgnore:section.gnu.version_r.alignment

int main() { return 42; }
