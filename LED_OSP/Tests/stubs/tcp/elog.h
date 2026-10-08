/* Consume arguments like the real logger without printing host test noise. */
static inline void test_elog_discard(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
#define elog_e(...) test_elog_discard(__VA_ARGS__)
#define elog_i(...) test_elog_discard(__VA_ARGS__)
#define elog_w(...) test_elog_discard(__VA_ARGS__)
#define elog_hexdump(...) ((void)0)
