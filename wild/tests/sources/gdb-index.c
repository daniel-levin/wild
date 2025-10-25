//#Config:clang
//#Compiler: clang
//#LinkerDriver:clang
//#LinkArgs:-z now -Wl,--gdb-index
//#SkipLinker:ld
//#EnableLinker:lld
//#DiffIgnore:section.relro_padding
//#DiffIgnore:section.got.plt.entsize
//#DiffIgnore:section.rodata
//#DiffIgnore:section.gnu.version_r.alignment
//#Contains:gdb_index

//#Compiler: clang
//#CompArgs:-ggdb -gdwarf-4
//#LinkerDriver:clang
//#LinkArgs:-z now -Wl,--gdb-index
//#SkipLinker:ld
//#EnableLinker:lld
//#DiffIgnore:section.relro_padding
//#DiffIgnore:section.got.plt.entsize
//#DiffIgnore:section.rodata
//#DiffIgnore:section.gnu.version_r.alignment

int main() { return 42; }
