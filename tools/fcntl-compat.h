/* glibc >= 2.28 redirects fcntl() to fcntl64() when _FILE_OFFSET_BITS=64 is
 * defined (which FFmpeg's configure does on Linux). fcntl64 is not exported
 * by every glibc (it was dropped on 64-bit architectures in newer glibc
 * releases), so libraries built with that redirect fail to load on hosts
 * whose libc lacks the symbol ("fcntl64: symbol not found").
 *
 * On 64-bit targets fcntl() already has 64-bit semantics, so removing the
 * redirect is safe and keeps the built libraries loadable on any glibc.
 */
#ifdef fcntl
#undef fcntl
#endif
