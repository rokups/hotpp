/* A test input of the symbol check: it needs dlopen and fork, so the check must fail. */
extern "C" void *dlopen(const char *, int);
extern "C" int fork(void);
int aot_only_bad_strong(void) { return dlopen("x", 0) != 0 || fork(); }
