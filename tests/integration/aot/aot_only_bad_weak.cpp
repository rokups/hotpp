/* A test input of the symbol check: it has a weak undefined popen, so the check must fail. */
extern "C" void *popen(const char *, const char *) __attribute__((weak));
int aot_only_bad_weak(void) { return popen != 0; }
